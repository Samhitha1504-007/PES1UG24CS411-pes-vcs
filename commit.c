// commit.c — Commit creation and history traversal
// Format: tree <hex>\n [parent <hex>\n] author <name> <ts>\n committer... \n\n <msg>
#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// Parse raw commit text into a Commit struct
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;
    const char *p = (const char *)data;
    char hex[HASH_HEX_SIZE + 1];

    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n') + 1;

    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        commit_out->has_parent = 0;
    }

    char author_buf[256]; uint64_t ts;
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;
    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;
    ts = (uint64_t)strtoull(last_space+1, NULL, 10);
    *last_space = '\0';
    snprintf(commit_out->author, sizeof(commit_out->author), "%s", author_buf);
    commit_out->timestamp = ts;
    p = strchr(p, '\n') + 1;  // skip author line
    p = strchr(p, '\n') + 1;  // skip committer line
    p = strchr(p, '\n') + 1;  // skip blank line
    snprintf(commit_out->message, sizeof(commit_out->message), "%s", p);
    return 0;
}

// TODO: commit_serialize
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    (void)commit; (void)data_out; (void)len_out; return -1;
}

// TODO: commit_walk
int commit_walk(commit_walk_fn callback, void *ctx) {
    (void)callback; (void)ctx; return -1;
}

// TODO: head_read / head_update / commit_create
int head_read(ObjectID *id_out) { (void)id_out; return -1; }
int head_update(const ObjectID *c) { (void)c; return -1; }
int commit_create(const char *msg, ObjectID *out) { (void)msg; (void)out; return -1; }
