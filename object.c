// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYYYY... where XX is the first two hex characters of the
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

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build the type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Step 2: Build the full header: "<type> <size>\0"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    // header_len does NOT include the null terminator written by snprintf,
    // but we want the '\0' in the stored object, so total = header_len + 1 + len
    size_t full_len = (size_t)header_len + 1 + len;

    // Step 3: Assemble full object in memory (header + '\0' + data)
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, (size_t)header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    // Step 4: Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full, full_len, &id);
    if (id_out) *id_out = id;

    // Step 5: Deduplication — if the object already exists, we're done
    if (object_exists(&id)) {
        free(full);
        return 0;
    }

    // Step 6: Build the shard directory path and create it
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // OK if it already exists

    // Step 7: Build final object path
    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    // Step 8: Write to a temporary file in the shard directory
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/.tmp_XXXXXX", OBJECTS_DIR, hex);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(full);
        return -1;
    }

    // Write the full object
    ssize_t written = write(fd, full, full_len);
    free(full);
    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // Step 9: fsync the temp file to ensure data is on disk
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // Step 10: Atomically rename temp file to final path
    if (rename(tmp_path, obj_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // Step 11: fsync the shard directory to persist the directory entry
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// Read an object from the store.
//
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
// The caller is responsible for calling free(*data_out).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Build the path from the hash
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // Get file size
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long file_size = ftell(f);
    if (file_size < 0) { fclose(f); return -1; }
    rewind(f);

    // Read full contents
    uint8_t *raw = malloc((size_t)file_size);
    if (!raw) { fclose(f); return -1; }
    if (fread(raw, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(raw);
        fclose(f);
        return -1;
    }
    fclose(f);

    // Step 3: Verify integrity — recompute hash and compare to requested hash
    ObjectID computed;
    compute_hash(raw, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(raw);
        return -1; // Data corrupted
    }

    // Step 4: Find the '\0' separator between header and data
    uint8_t *null_byte = memchr(raw, '\0', (size_t)file_size);
    if (!null_byte) {
        free(raw);
        return -1;
    }

    // Step 5: Parse header — "<type> <size>"
    size_t header_len = (size_t)(null_byte - raw);
    char header[64];
    if (header_len >= sizeof(header)) { free(raw); return -1; }
    memcpy(header, raw, header_len);
    header[header_len] = '\0';

    // Parse type
    ObjectType parsed_type;
    if      (strncmp(header, "blob ",   5) == 0) parsed_type = OBJ_BLOB;
    else if (strncmp(header, "tree ",   5) == 0) parsed_type = OBJ_TREE;
    else if (strncmp(header, "commit ", 7) == 0) parsed_type = OBJ_COMMIT;
    else { free(raw); return -1; }

    // Parse size — the number after the first space
    char *space = strchr(header, ' ');
    if (!space) { free(raw); return -1; }
    size_t declared_size = (size_t)strtoul(space + 1, NULL, 10);

    // Validate the declared size matches what's actually after the '\0'
    size_t data_offset = header_len + 1;
    size_t actual_data_len = (size_t)file_size - data_offset;
    if (actual_data_len != declared_size) {
        free(raw);
        return -1;
    }

    // Step 6: Allocate and return the data portion
    uint8_t *data_copy = malloc(declared_size + 1); // +1 for safety null terminator
    if (!data_copy) { free(raw); return -1; }
    memcpy(data_copy, raw + data_offset, declared_size);
    data_copy[declared_size] = '\0'; // Safe null-terminate (not counted in len)

    free(raw);

    if (type_out)  *type_out  = parsed_type;
    if (data_out)  *data_out  = data_copy;
    if (len_out)   *len_out   = declared_size;

    return 0;
}
