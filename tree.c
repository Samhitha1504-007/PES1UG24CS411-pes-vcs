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

#include "tree.h"
#include "index.h"
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

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Forward declarations
int index_load(Index *index);
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Recursive helper: builds a tree level from a slice of index entries that
// all share the same directory prefix at the given depth level.
//
// entries: sorted array of IndexEntry pointers for this subtree
// count:   number of entries in this slice
// depth:   current path depth (0 = root, 1 = one level deep, etc.)
// id_out:  receives the hash of the written tree object
//
// Returns 0 on success, -1 on error.
static int write_tree_level(IndexEntry **entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        IndexEntry *e = entries[i];

        // Find the component at the current depth level in the path
        // e.g. depth=0: "src/main.c" → component is "src", "README.md" → component is "README.md"
        const char *path = e->path;
        const char *component = path;

        // Skip the first `depth` path components
        for (int d = 0; d < depth; d++) {
            const char *slash = strchr(component, '/');
            if (!slash) {
                component = NULL;
                break;
            }
            component = slash + 1;
        }

        if (!component) { i++; continue; }

        // Check if there's a slash after this component (meaning it's a directory at this level)
        const char *slash = strchr(component, '/');

        if (!slash) {
            // It's a file at this directory level — add it directly as a blob entry
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = e->mode;
            te->hash = e->hash;
            strncpy(te->name, component, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // It's a subdirectory at this level — gather all entries with this prefix
            size_t dir_name_len = (size_t)(slash - component);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, component, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Count how many entries share this subdirectory
            int j = i;
            while (j < count) {
                IndexEntry *ej = entries[j];
                const char *comp_j = ej->path;
                for (int d = 0; d < depth; d++) {
                    const char *s = strchr(comp_j, '/');
                    if (!s) { comp_j = NULL; break; }
                    comp_j = s + 1;
                }
                if (!comp_j) break;
                const char *sl = strchr(comp_j, '/');
                if (!sl) break; // This file is at this level, not in the subdir
                size_t this_dir_len = (size_t)(sl - comp_j);
                if (this_dir_len != dir_name_len ||
                    strncmp(comp_j, dir_name, dir_name_len) != 0) break;
                j++;
            }

            // Recursively build the subtree for entries[i..j-1]
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, depth + 1, &sub_id) != 0) return -1;

            // Add a tree entry for this subdirectory
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j;
        }
    }

    // Serialize this tree and write it to the object store
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty index — write an empty tree
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    // Build an array of pointers to entries (already sorted by path in index)
    IndexEntry *entry_ptrs[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index.count; i++) {
        entry_ptrs[i] = &index.entries[i];
    }

    return write_tree_level(entry_ptrs, index.count, 0, id_out);
}