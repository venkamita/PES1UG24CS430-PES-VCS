// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA‑256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────
#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────
// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Find the space separating mode from name
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;                     // malformed

        // Parse mode (octal)
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;                           // skip space

        // 2. Find the NUL terminator after the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;                 // malformed
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;                       // skip NUL

        // 3. Read the 32‑byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort – ensures deterministic ordering for hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name,
                  ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into the binary format used for storage.
// Caller must free(*data_out). Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Rough upper bound: (mode + space + name + NUL + hash) per entry
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Sort a mutable copy (Git requires sorted entries)
    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", e->mode, e->name);
        offset += written + 1;                     // +1 for the NUL written by sprintf
        memcpy(buffer + offset, e->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out   = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#include "index.h"

// Recursive helper that builds a tree object from a sorted slice of index entries.
static int write_tree_recursive(IndexEntry *entries, int count,
                                const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    int prefix_len = prefix ? (int)strlen(prefix) : 0;

    int i = 0;
    while (i < count) {
        const char *rel_path = entries[i].path + prefix_len;
        char *slash = strchr(rel_path, '/');

        if (!slash) {
            // Plain file at this level
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel_path, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // Directory – gather all entries belonging to it
            char dir_name[256];
            size_t dir_len = slash - rel_path;
            if (dir_len >= sizeof(dir_name)) dir_len = sizeof(dir_name) - 1;
            strncpy(dir_name, rel_path, dir_len);
            dir_name[dir_len] = '\0';

            // New prefix for the recursive call (e.g. "src/" )
            char new_prefix[512];
            if (prefix)
                snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);
            else
                snprintf(new_prefix, sizeof(new_prefix), "%s/", dir_name);

            // Count how many consecutive entries share this sub‑directory
            int sub_start = i;
            int sub_count = 0;
            while (i < count) {
                const char *p = entries[i].path + prefix_len;
                if (strncmp(p, dir_name, dir_len) == 0 && p[dir_len] == '/') {
                    sub_count++;
                    i++;
                } else {
                    break;
                }
            }

            // Recurse to create the subtree object
            ObjectID subtree_id;
            if (write_tree_recursive(entries + sub_start, sub_count,
                                     new_prefix, &subtree_id) != 0)
                return -1;

            // Add a directory entry that points to the subtree
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        }
    }

    // Serialize the constructed tree and store it as an OBJ_TREE
    void *tree_data = NULL;
    size_t tree_len = 0;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build the full tree hierarchy from the current index and write it.
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) return -1;

    return write_tree_recursive(index.entries, index.count, NULL, id_out);
}

