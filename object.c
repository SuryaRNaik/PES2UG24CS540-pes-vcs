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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str;

    // 1. Convert enum to string
    switch (type) {
        case OBJ_BLOB: type_str = "blob"; break;
        case OBJ_TREE: type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 2. Build header
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // 3. Build full object buffer
    size_t total_len = header_len + len;
    unsigned char *buffer = malloc(total_len);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, len);

    // 4. Compute hash
    compute_hash(buffer, total_len, id_out);

    // 5. Dedup check
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // 6. Create directory
    char path[512];
    object_path(id_out, path, sizeof(path));

    char dir[512];
    strncpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(buffer);
        return -1;
    }
    *slash = '\0';

    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    // 7. Temp file path
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/tmpXXXXXX", dir);

    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(buffer);
        return -1;
    }
}

// Read an object from the store.
//
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

    // ───────────── Step 1: Build path ─────────────
    char path[512];
    object_path(id, path, sizeof(path));


    // ───────────── Step 2: Open file ─────────────
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }


    // ───────────── Step 3: Get file size ─────────────
    fseek(fp, 0, SEEK_END);

    long file_size = ftell(fp);

    rewind(fp);

    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }


    // ───────────── Step 4: Allocate buffer ─────────────
    unsigned char *buffer = (unsigned char *)malloc(file_size);

    if (!buffer) {
        fclose(fp);
        return -1;
    }


    // ───────────── Step 5: Read file into buffer ─────────────
    size_t read_bytes = fread(buffer, 1, file_size, fp);

    if (read_bytes != (size_t)file_size) {
        free(buffer);
        fclose(fp);
        return -1;
    }

    fclose(fp);


    // ───────────── Step 6: Verify hash ─────────────
    ObjectID computed;

    compute_hash(buffer, file_size, &computed);

    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }


    // ───────────── Step 7: Find header separator ─────────────
    unsigned char *null_ptr = memchr(buffer, '\0', file_size);

    if (!null_ptr) {
        free(buffer);
        return -1;
    }


    // ───────────── Step 8: Extract header ─────────────
    size_t header_len = (size_t)(null_ptr - buffer);

    char *header = (char *)buffer;


    // ───────────── Step 9: Parse object type ─────────────
    if (strncmp(header, "blob", 4) == 0) {
        *type_out = OBJ_BLOB;
    }
    else if (strncmp(header, "tree", 4) == 0) {
        *type_out = OBJ_TREE;
    }
    else if (strncmp(header, "commit", 6) == 0) {
        *type_out = OBJ_COMMIT;
    }
    else {
        free(buffer);
        return -1;
    }


    // ───────────── Step 10: Parse size from header ─────────────
    size_t size;

    int parsed = sscanf(header, "%*s %zu", &size);

    if (parsed != 1) {
        free(buffer);
        return -1;
    }


    // ───────────── Step 11: Locate data section ─────────────
    unsigned char *data_start = null_ptr + 1;


    // ───────────── Step 12: Bounds check ─────────────
    size_t total_required = (size_t)(data_start - buffer) + size;

    if (total_required > (size_t)file_size) {
        free(buffer);
        return -1;
    }


    // ───────────── Step 13: Allocate output buffer ─────────────
    void *out = malloc(size);

    if (!out) {
        free(buffer);
        return -1;
    }


    // ───────────── Step 14: Copy data ─────────────
    memcpy(out, data_start, size);


    // ───────────── Step 15: Set outputs ─────────────
    *data_out = out;
    *len_out = size;


    // ───────────── Step 16: Cleanup ─────────────
    free(buffer);

    return 0;
}
