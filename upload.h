#ifndef UPLOAD_H
#define UPLOAD_H

#include <stdint.h>
#include "common.h"

typedef struct {
    uint32_t index;
    uint32_t size;
    char hash[HASH_HEX_LEN + 1];
} chunk;

typedef struct {
    char filename[256];
    uint64_t total_size;
    uint32_t chunk_size;
    char hash_algo[32];
    char temp_manifest_path[512];

    chunk *chunks;
    uint32_t chunk_count;
    uint32_t chunk_cap;

    char cid[HASH_HEX_LEN + 1];

    uint32_t next_index;

    pthread_mutex_t lock;
    uint32_t tasks_in_progress;
    pthread_cond_t finished_cond;

} upload_S;

//for each chanck in thread pool
typedef struct {
    uint32_t chunk_index;
    uint32_t chunk_len;
    uint8_t *data;

    char hash_hex[HASH_HEX_LEN + 1];

    upload_S *session;
} upload_task_arg_t;

void handle_upload_start(int cfd, upload_S *sess, const uint8_t *payload, uint32_t len);
void handle_upload_chunk(int cfd, upload_S *sess, const uint8_t *payload, uint32_t len);
void handle_upload_finish(int cfd, upload_S *sess);

#endif
