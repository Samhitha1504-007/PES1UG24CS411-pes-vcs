// index.c — Staging area implementation
// Text format: <mode-octal> <64-hex-hash> <mtime> <size> <path>
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i+1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path); staged++;
    }
    if (!staged) printf("  (nothing to show)\n");
    printf("\n");
    printf("Unstaged changes:\n");
    int unstaged = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path); unstaged++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size  != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path); unstaged++;
        }
    }
    if (!unstaged) printf("  (nothing to show)\n");
    printf("\n");
    printf("Untracked files:\n");
    int untracked = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name,".") == 0 || strcmp(ent->d_name,"..") == 0) continue;
            if (strcmp(ent->d_name,".pes") == 0) continue;
            if (strcmp(ent->d_name,"pes") == 0) continue;
            if (strstr(ent->d_name,".o") != NULL) continue;
            int tracked = 0;
            for (int i = 0; i < index->count; i++)
                if (strcmp(index->entries[i].path, ent->d_name) == 0) { tracked=1; break; }
            if (!tracked) {
                struct stat st; stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { printf("  untracked:  %s\n", ent->d_name); untracked++; }
            }
        }
        closedir(dir);
    }
    if (!untracked) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// Load the index from .pes/index text file into memory.
// Missing file is not an error - repo may have no staged files yet.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // No index yet: start empty

    char hex[HASH_HEX_SIZE + 4];
    char path[512];
    uint32_t mode, size;
    uint64_t mtime;

    while (index->count < MAX_INDEX_ENTRIES) {
        int ret = fscanf(f, "%o %64s %llu %u %511s\n",
                         &mode, hex, (unsigned long long *)&mtime, &size, path);
        if (ret == EOF) break;
        if (ret != 5) { int c; while ((c=fgetc(f)) != '\n' && c != EOF); continue; }

        IndexEntry *e = &index->entries[index->count];
        e->mode = mode;
        if (hex_to_hash(hex, &e->hash) != 0) continue;
        e->mtime_sec = mtime;
        e->size = size;
        strncpy(e->path, path, sizeof(e->path)-1);
        e->path[sizeof(e->path)-1] = '\0';
        index->count++;
    }
    fclose(f);
    return 0;
}

// TODO: index_save
int index_save(const Index *index) {
    (void)index; return -1;
}

// TODO: index_add
int index_add(Index *index, const char *path) {
    (void)index; (void)path; return -1;
}
