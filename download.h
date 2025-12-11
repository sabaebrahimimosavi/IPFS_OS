#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdint.h>
#include <pthread.h>
#include "upload.h"
#include "common.h"

// A chunk that is fully read+verified and ready to be sent.
typedef struct {
    uint32_t index;
    uint32_t size;
    uint8_t *data;
} ready_chunk_t;

// Aggregator state for a single download session.
// One per download request.
typedef struct {
    int cfd;                    // connection fd to client
    uint32_t next_index_to_send;
    uint32_t total_chunks;

    ready_chunk_t *buffers;     // dynamic array of ready chunks
    uint32_t buffer_cap;
    uint32_t buffer_count;

    int has_error;              // set to 1 if any worker fails

    chunk *manifest_chunks;     // parsed manifest array (length = total_chunks)

    pthread_mutex_t lock;
    pthread_cond_t ready_cond;
} download_aggregator_t;

// Argument to worker tasks in the thread pool.
typedef struct {
    download_aggregator_t *agg;
    const chunk *ci;
} chunk_task_arg_t;

int parse_manifest_chunks(const uint8_t *buf, size_t buf_length, chunk **output_chunks);
void handle_download(int cfd, const uint8_t *payload, uint32_t len);

#endif