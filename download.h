#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "upload.h"

typedef struct {
    uint32_t index;
    uint32_t size;
    uint8_t *data; 
} ready_chunk_t;

typedef struct {
    int cfd; //connection to client
    uint32_t next_index_to_send;
    uint32_t total_chunks;

    ready_chunk_t *buffers;//chuncks that has been prepered by workers
    int buffer_cap;
    uint32_t buffer_count; 

    int has_error; 

    chunk *manifest_chunks;
    
    pthread_mutex_t lock;
    pthread_cond_t ready_cond;
} download_aggregator_t;

typedef struct {
    download_aggregator_t *agg; 
    const chunk *ci;          
} chunk_task_arg_t;

int parse_manifest_chunks(const uint8_t *buf , size_t buf_length , chunk **output_chunks);
int send_block_chunk(int cfd, const chunk *ci);
void handle_download(int cfd, const uint8_t *payload, uint32_t len);

#endif
