// ───────────── Header Files for Index Module ─────────────

// Provides definitions for Index, IndexEntry, and related functions
#include "index.h"

// Provides core PES structures and object storage functions (e.g., object_write)
#include "pes.h"


// ───────────── Standard Library Headers ─────────────

// Input/output operations (fopen, fread, fwrite, printf, etc.)
#include <stdio.h>

// Memory management (malloc, free, exit)
#include <stdlib.h>

// String manipulation functions (strcmp, strcpy, strlen, memcpy, etc.)
#include <string.h>


// ───────────── System Headers ─────────────

// File metadata operations (stat, file size, modification time)
#include <sys/stat.h>

// File control options (low-level file descriptors, flags)
#include <fcntl.h>

// POSIX system calls (read, write, close, access, etc.)
#include <unistd.h>

// Directory handling (opendir, readdir, closedir)
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *idx) {

    // Open index file in binary read mode
    FILE *fp = fopen(".pes/index", "rb");


    // ───────────── Case: Index file does not exist ─────────────
    // This happens on first run (no files staged yet)
    if (!fp) {
        idx->count = 0;   // Initialize empty index
        return 0;
    }


    // ───────────── Step 1: Read number of entries ─────────────
    fread(&idx->count, sizeof(int), 1, fp);


    // ───────────── Step 2: Validate entry count ─────────────
    // Prevent overflow or corrupted index file
    if (idx->count > MAX_INDEX_ENTRIES) {
        fclose(fp);
        return -1;
    }


    // ───────────── Step 3: Read all entries ─────────────
    fread(
        idx->entries,
        sizeof(IndexEntry),
        idx->count,
        fp
    );


    // ───────────── Step 4: Close file ─────────────
    fclose(fp);


    // ───────────── Load successful ─────────────
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *idx) {

    // Open index file in binary write mode
    FILE *fp = fopen(".pes/index", "wb");

    // Check if file opening failed
    if (!fp) {
        return -1;
    }


    // ───────────── Step 1: Write entry count ─────────────
    fwrite(
        &idx->count,
        sizeof(int),
        1,
        fp
    );
    fwrite(
        idx->entries,
        sizeof(IndexEntry),
        idx->count,
        fp
    );


    // ───────────── Step 3: Close file ─────────────
    fclose(fp);


    // ───────────── Save successful ─────────────
    return 0;

}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    FILE *fp = fopen(path, "rb");

    if (!fp) {
        return -1;   // File could not be opened
    }


    // ───────────── Step 2: Determine file size ─────────────
    fseek(fp, 0, SEEK_END);

    long size = ftell(fp);

    rewind(fp);


    // ───────────── Step 3: Allocate buffer ─────────────
    void *data = malloc(size);

    if (!data) {
        fclose(fp);
        return -1;
    }


    // ───────────── Step 4: Read file content ─────────────
    fread(data, 1, size, fp);

    fclose(fp);


    // ───────────── Step 5: Store file as blob object ─────────────
    ObjectID id;

    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }

    // Free temporary buffer
    free(data);


    // ───────────── Step 6: Get file metadata ─────────────
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }


    // ───────────── Step 7: Check if file already exists in index ─────────────
    for (int i = 0; i < idx->count; i++) {

        if (strcmp(idx->entries[i].path, path) == 0) {

            // Update existing entry
            idx->entries[i].hash = id;
            idx->entries[i].size = st.st_size;
            idx->entries[i].mtime_sec = st.st_mtime;
            idx->entries[i].mode = st.st_mode;

            // Save updated index to disk
            return index_save(idx);   // 🔥 critical
        }
    }
    if (idx->count >= MAX_INDEX_ENTRIES) {
        return -1;   // Prevent overflow
    }


    // Store file path
    strcpy(idx->entries[idx->count].path, path);

    // Store hash (blob reference)
    idx->entries[idx->count].hash = id;

    // Store metadata
    idx->entries[idx->count].size = st.st_size;
    idx->entries[idx->count].mtime_sec = st.st_mtime;
    idx->entries[idx->count].mode = st.st_mode;


    // Increment entry count
    idx->count++;


    // ───────────── Step 9: Save index to disk ─────────────
    // This ensures persistence since pes.c does NOT call index_save()
    return index_save(idx);
}
