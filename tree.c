// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

// tree.c — Tree object serialization and construction
#include "tree.h"
#include <index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(Index *index);

// ─── Mode Constants ──────────────────────────────────────────────────────────
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count,
          sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset,
                              "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

static int cmp_entry_ptrs(const void *a, const void *b) {
    return strcmp((*(IndexEntry **)a)->path, (*(IndexEntry **)b)->path);
}

// Recursive helper: builds a tree for entries under `prefix`
// e.g. prefix="" for root, prefix="src/" for the src subdirectory
static int write_tree_level(IndexEntry **entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    size_t prefix_len = strlen(prefix);

    int i = 0;
    while (i < count) {
        // Path relative to current directory level
        const char *rel = entries[i]->path + prefix_len;
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // Plain file at this level — add as blob entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = entries[i]->hash;
            i++;
        } else {
            // Subdirectory — get its name
            size_t dir_len = (size_t)(slash - rel);
            char dir_name[256];
            strncpy(dir_name, rel, dir_len);
            dir_name[dir_len] = '\0';

            // Build prefix for recursive call e.g. "src/"
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);
            size_t new_prefix_len = strlen(new_prefix);

            // Collect all entries that belong to this subdirectory
            int j = i;
            while (j < count &&
                   strncmp(entries[j]->path, new_prefix, new_prefix_len) == 0) {
                j++;
            }

            // Recurse into subdirectory
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, new_prefix, &sub_id) != 0)
                return -1;

            // Add subtree entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = sub_id;

            i = j;
        }
    }

    // Serialize and write this tree to the object store
    void *tdata;
    size_t tlen;
    if (tree_serialize(&tree, &tdata, &tlen) != 0) return -1;
    int rc = object_write(OBJ_TREE, tdata, tlen, id_out);
    free(tdata);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    index.count = 0;
    index_load(&index);  // empty index is fine (first commit)

    // Sort entries by path
    IndexEntry *ptrs[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index.count; i++)
        ptrs[i] = &index.entries[i];
    qsort(ptrs, index.count, sizeof(IndexEntry *), cmp_entry_ptrs);

    return write_tree_level(ptrs, index.count, "", id_out);
}
