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
#include "pes.h"
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

    // Retrieve file metadata
    if (lstat(path, &st) != 0) {
        return 0;
    }

    // Check if path is a directory
    if (S_ISDIR(st.st_mode)) {
        return MODE_DIR;
    }

    // Check if executable bit is set
    if (st.st_mode & S_IXUSR) {
        return MODE_EXEC;
    }

    // Default: regular file
    return MODE_FILE;
}

// ───────────── Parse Tree Object ─────────────
// Converts raw binary tree data into a Tree struct
int tree_parse(const void *data, size_t len, Tree *tree_out) {

    // Initialize tree entry count
    tree_out->count = 0;

    // Set pointer to start of data
    const uint8_t *ptr = (const uint8_t *)data;

    // Define end pointer for bounds checking
    const uint8_t *end = ptr + len;


    // ───────────── Iterate through entries ─────────────
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {

        // Get reference to next tree entry
        TreeEntry *entry = &tree_out->entries[tree_out->count];


        // ───────────── Step 1: Parse mode ─────────────

        // Locate space separating mode and name
        const uint8_t *space = memchr(ptr, ' ', end - ptr);

        if (!space) {
            return -1;   // Malformed entry
        }

        // Extract mode string
        char mode_str[16] = {0};

        size_t mode_len = (size_t)(space - ptr);

        // Ensure mode string fits buffer
        if (mode_len >= sizeof(mode_str)) {
            return -1;
        }

        // Copy mode string
        memcpy(mode_str, ptr, mode_len);

        // Convert string → integer (octal)
        entry->mode = strtol(mode_str, NULL, 8);


        // Move pointer past space
        ptr = space + 1;


        // ───────────── Step 2: Parse name ─────────────

        // Find null terminator after name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);

        if (!null_byte) {
            return -1;   // Malformed entry
        }

        // Calculate name length
        size_t name_len = (size_t)(null_byte - ptr);

        // Ensure name fits buffer
        if (name_len >= sizeof(entry->name)) {
            return -1;
        }

        // Copy name
        memcpy(entry->name, ptr, name_len);

        // Add null terminator
        entry->name[name_len] = '\0';


        // Move pointer past null byte
        ptr = null_byte + 1;


        // ───────────── Step 3: Parse hash ─────────────

        // Ensure enough bytes remain for hash
        if (ptr + HASH_SIZE > end) {
            return -1;
        }

        // Copy hash (raw 32 bytes)
        memcpy(entry->hash.hash, ptr, HASH_SIZE);

        // Advance pointer
        ptr += HASH_SIZE;


        // ───────────── Step 4: Increment entry count ─────────────
        tree_out->count++;//incrementing
    }


    // ───────────── Parsing complete ─────────────
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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
static int build_tree(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {

    Tree tree;
    tree.count = 0;

    int i = 0;


    // ───────────── Step 2: Iterate through entries ─────────────
    while (i < count) {

        // Get full file path
        const char *path = entries[i].path;

        // Relative path (after removing prefix)
        const char *rel = path;


        // ───────────── Step 2A: Strip prefix ─────────────
        if (prefix && strlen(prefix) > 0) {

            // Skip entries not belonging to current prefix
            if (strncmp(path, prefix, strlen(prefix)) != 0) {
                i++;
                continue;
            }

            // Move pointer to relative part
            rel = path + strlen(prefix);
        }


        // ───────────── Step 2B: Check for subdirectory ─────────────
        char *slash = strchr(rel, '/');


        // ───────────── CASE 1: FILE (no slash) ─────────────
        if (!slash) {

            // Add file entry directly to tree
            TreeEntry *entry = &tree.entries[tree.count++];

            entry->mode = MODE_FILE;

            // Store file name
            strcpy(entry->name, rel);

            // Store file hash (blob)
            entry->hash = entries[i].hash;

            // Move to next entry
            i++;
        }


        // ───────────── CASE 2: DIRECTORY (has slash) ─────────────
        else {

            // ───────────── Extract directory name ─────────────
            char dirname[256];

            int len = slash - rel;

            strncpy(dirname, rel, len);

            dirname[len] = '\0';   // null terminate


            // ───────────── Step 2C: Collect sub-entries ─────────────
            IndexEntry sub_entries[256];
            int sub_count = 0;

            int j = i;

            while (j < count) {

                const char *p = entries[j].path;

                // Apply prefix stripping again
                if (prefix && strlen(prefix) > 0) {
                    if (strncmp(p, prefix, strlen(prefix)) != 0) {
                        j++;
                        continue;
                    }
                    p += strlen(prefix);
                }

                // Check if entry belongs to this directory
                if (strncmp(p, dirname, len) == 0 && p[len] == '/') {
                    sub_entries[sub_count++] = entries[j];
                }

                j++;
            }
/ ───────────── Step 2D: Build new prefix ─────────────
            char new_prefix[512];

            if (prefix && strlen(prefix) > 0) {
                snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dirname);
            } else {
                snprintf(new_prefix, sizeof(new_prefix), "%s/", dirname);
            }


            // ───────────── Step 2E: Recursive call ─────────────
            ObjectID sub_id;

            if (build_tree(sub_entries, sub_count, new_prefix, &sub_id) != 0) {
                return -1;
            }


            // ───────────── Step 2F: Add directory entry ─────────────
            TreeEntry *entry = &tree.entries[tree.count++];

            entry->mode = MODE_DIR;

            strcpy(entry->name, dirname);

            entry->hash = sub_id;


            // Skip all processed entries
            i += sub_count;
        }
    }


    // ───────────── Step 3: Serialize tree ─────────────
    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0) {
        return -1;
    }


    // ───────────── Step 4: Write tree object ─────────────
    if (object_write(OBJ_TREE, data, len, id_out) != 0) {
        free(data);
        return -1;
    }


    // ───────────── Step 5: Cleanup ─────────────
    free(data);


    // ───────────── Step 6: Success ─────────────
    return 0;
}

int tree_from_index(ObjectID *id_out) {

    // ───────────── Step 1: Declare Index structure ─────────────
    // This will hold all staged entries (files added via `pes add`)
    Index idx;


    // ───────────── Step 2: Load index from disk ─────────────
    // Reads .pes/index into memory
    int load_status = index_load(&idx);

    // If loading fails, return error
    if (load_status != 0) {
        return -1;
    }


    // ───────────── Step 3: Build tree recursively ─────────────
    // Pass:
    //   - all index entries
    //   - total number of entries
    //   - empty prefix (root level)
    //   - output ObjectID for resulting tree
    int result = build_tree(
        idx.entries,
        idx.count,
        "",
        id_out
    );


    // ───────────── Step 4: Return result ─────────────
    // Success (0) or failure (-1)
    return result;
}
