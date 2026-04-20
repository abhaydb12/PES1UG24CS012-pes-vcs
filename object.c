// object.c — Content-addressable object store
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>

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
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

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

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header e.g. "blob 16\0"
    const char *type_str = (type == OBJ_BLOB)   ? "blob"   :
                           (type == OBJ_TREE)   ? "tree"   : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // 2. Concatenate header + data into one buffer
    size_t full_size = (size_t)hlen + len;
    unsigned char *full = malloc(full_size);
    if (!full) {
        fprintf(stderr, "object_write: malloc failed\n");
        return -1;
    }
    memcpy(full, header, (size_t)hlen);
    if (len > 0) memcpy(full + hlen, data, len);

    // 3. SHA-256 hash the full object
    compute_hash(full, full_size, id_out);

    // 4. Deduplication
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // 5. Build paths
    char obj_path[512];
    object_path(id_out, obj_path, sizeof(obj_path));

    // Extract shard directory by finding last '/'
    char shard_dir[512];
    strncpy(shard_dir, obj_path, sizeof(shard_dir) - 1);
    shard_dir[sizeof(shard_dir) - 1] = '\0';
    char *last_slash = strrchr(shard_dir, '/');
    if (!last_slash) {
        fprintf(stderr, "object_write: bad path %s\n", obj_path);
        free(full);
        return -1;
    }
    *last_slash = '\0';

    // 6. Create shard directory (.pes/objects/ab/)
    if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "object_write: mkdir(%s) failed: %s\n", shard_dir, strerror(errno));
        free(full);
        return -1;
    }

    // 7. Write to temp file then rename (atomic write)
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", obj_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "object_write: open(%s) failed: %s\n", tmp_path, strerror(errno));
        free(full);
        return -1;
    }

    size_t written = 0;
    while (written < full_size) {
        ssize_t n = write(fd, (char*)full + written, full_size - written);
        if (n <= 0) {
            fprintf(stderr, "object_write: write failed: %s\n", strerror(errno));
            close(fd);
            free(full);
            return -1;
        }
        written += (size_t)n;
    }
    fsync(fd);
    close(fd);
    free(full);

    // 8. Atomic rename
    if (rename(tmp_path, obj_path) != 0) {
        fprintf(stderr, "object_write: rename failed: %s\n", strerror(errno));
        return -1;
    }

    // 9. fsync the shard directory
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

    // 2. Read entire file
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "object_read: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return -1; }

    unsigned char *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }

    if ((long)fread(buf, 1, (size_t)fsize, f) != fsize) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);

    // 3. Integrity check
    ObjectID computed;
    compute_hash(buf, (size_t)fsize, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        fprintf(stderr, "object_read: integrity check failed for %s\n", path);
        free(buf);
        return -1;
    }

    // 4. Find null byte separating header from data
    unsigned char *null_pos = memchr(buf, '\0', (size_t)fsize);
    if (!null_pos) {
        fprintf(stderr, "object_read: no null byte found in header\n");
        free(buf);
        return -1;
    }

    // 5. Parse type
    if      (strncmp((char*)buf, "blob ",   5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ",   5) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else {
        fprintf(stderr, "object_read: unknown type in header\n");
        free(buf);
        return -1;
    }

    // 6. Return data portion (after the \0)
    size_t header_size = (size_t)(null_pos - buf) + 1;
    *len_out  = (size_t)fsize - header_size;
    *data_out = malloc(*len_out > 0 ? *len_out : 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, buf + header_size, *len_out);
    free(buf);

    return 0;
}
