// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>
// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header string: "blob 16\0"
    const char *type_str = (type == OBJ_BLOB)   ? "blob"   :
                           (type == OBJ_TREE)   ? "tree"   : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
    // +1 to include the null terminator that snprintf writes

    // 2. Allocate and build full object = header + data
    size_t full_size = (size_t)hlen + len;
    unsigned char *full = malloc(full_size);
    if (!full) return -1;
    memcpy(full, header, hlen);
    memcpy(full + hlen, data, len);

    // 3. Hash the full object
    compute_hash(full, full_size, id_out);

    // 4. Deduplication — already stored, nothing to do
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // 5. Build shard dir path: ".pes/objects/ab"
    char obj_path[512];
    object_path(id_out, obj_path, sizeof(obj_path));

    // Extract the directory by zeroing out after last slash
    char shard_dir[512];
    strncpy(shard_dir, obj_path, sizeof(shard_dir) - 1);
    char *last_slash = strrchr(shard_dir, '/');
    if (!last_slash) { free(full); return -1; }
    *last_slash = '\0';

    // Create shard dir (ignore EEXIST)
    if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    // 6. Write to temp file (use 0644 so it can be overwritten if needed)
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", obj_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    // Write all bytes (handle partial writes)
    size_t written = 0;
    while (written < full_size) {
        ssize_t n = write(fd, (char*)full + written, full_size - written);
        if (n <= 0) { close(fd); free(full); return -1; }
        written += n;
    }

    fsync(fd);
    close(fd);
    free(full);

    // 7. Atomic rename: temp → final path
    if (rename(tmp_path, obj_path) != 0) return -1;

    // 8. fsync the shard directory to persist the rename
    int dfd = open(shard_dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out,
                void **data_out, size_t *len_out) {
    // 1. Get file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read entire file into buffer
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return -1; }

    unsigned char *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);

    // 3. Integrity check: recompute hash and compare to expected id
    ObjectID computed;
    compute_hash(buf, (size_t)fsize, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;  // Corrupted
    }

    // 4. Find the null byte separating header from data
    unsigned char *null_pos = memchr(buf, '\0', (size_t)fsize);
    if (!null_pos) { free(buf); return -1; }

    // 5. Parse type from header (before the space)
    if (strncmp((char*)buf, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // 6. Extract and return the data portion (everything after the \0)
    size_t header_size = (size_t)(null_pos - buf) + 1; // include the \0
    *len_out  = (size_t)fsize - header_size;
    *data_out = malloc(*len_out > 0 ? *len_out : 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, buf + header_size, *len_out);
    free(buf);

    return 0;
}
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // TODO: Implement
    (void)id; (void)type_out; (void)data_out; (void)len_out;
    return -1;
}
