#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "download.h"
#include "common.h"

extern void send_error(int fd, const char *code, const char *message);
extern int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);
extern thread_pool_t g_pool;

#define DOWNLOAD_BUF_SZ (256 * 1024)
#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

static void remove_ready_chunk(download_aggregator_t *agg, uint32_t index);
void* download_sender_loop(void* arg);
void handle_download(int cfd, const uint8_t *payload, uint32_t len);
static ready_chunk_t* find_ready_chunk(download_aggregator_t *agg, uint32_t index);
void aggregator_add_chunk(download_aggregator_t *agg, ready_chunk_t *rc);
void process_chunk_task(void* arg) ;

// json to chunk[]
int parse_manifest_chunks(const uint8_t *buf , size_t buf_length , chunk **output_chunks){
    const char *s = (const char*)buf;

    // look for substring "chunks" in manifest
    const char *chunks_pos = strstr(s, "\"chunks\"");
    
    if (!chunks_pos) return -1;
    const char *arr_start = strchr(chunks_pos, '[');
    if (!arr_start) return -1;
    const char *p = arr_start + 1;
    
    // space for 16 chunk for starter
    uint32_t cap = 16;
    uint32_t count = 0;
    chunk *arr = malloc(cap * sizeof(chunk));
    if (!arr) return -1;

    while (1){
        //look for "{" start of chunk info until it reach "}"
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) { free(arr); return -1; }
        
        // chunk info -> {"index":..., "size":..., "hash":"..."}
        const char *idx_pos  = strstr(obj, "\"index\"");
        const char *size_pos = strstr(obj, "\"size\"");
        const char *hash_pos = strstr(obj, "\"hash\"");
        if (!idx_pos || !size_pos || !hash_pos) {
            free(arr);
            return -1;
        }
        unsigned long idx_val = 0, size_val = 0;
        
        // skip all character until first digit then read it as index value(idx_val)
        if (sscanf(idx_pos, "\"index\"%*[^0-9]%lu", &idx_val) != 1) {
            free(arr);
            return -1;
        }
        
        // read the size (same logic as reading index)
        if (sscanf(size_pos, "\"size\"%*[^0-9]%lu", &size_val) != 1) {
            free(arr);
            return -1;
        }

        const char *q = strchr(hash_pos, '"');
        if (!q) {
            free(arr);
            return -1;
        }
        
        q = strchr(q + 1, '"');
        if (!q) {
            free(arr);
            return -1;
        }
        
        const char *val_start = strchr(q + 1, '"');
        if (!val_start) {
            free(arr);
            return -1;
        }
        val_start++;

        const char *val_end = strchr(val_start, '"');
        if (!val_end) {
            free(arr);
            return -1;
        }
        size_t h_len = (size_t) (val_end - val_start);
        if (h_len != HASH_HEX_LEN){
            if (h_len > HASH_HEX_LEN) h_len = HASH_HEX_LEN;
        }
        if (count == cap) {
            uint32_t ncap = cap * 2;
            chunk *narr = realloc(arr, ncap * sizeof(chunk));
            if (!narr) { free(arr); return -1; }
            arr = narr;
            cap = ncap;
        }
        arr[count].index = (uint32_t)idx_val;
        arr[count].size  = (uint32_t)size_val;

        memset(arr[count].hash, 0, HASH_HEX_LEN + 1);
        memcpy(arr[count].hash, val_start, h_len);
        arr[count].hash[HASH_HEX_LEN] = '\0';
        
        // increment count of parsed chunks
        count++;
        //move the pointer
        p= obj_end +1;
    }
    *output_chunks = arr;
    return (int) count;
}

static void remove_ready_chunk(download_aggregator_t *agg, uint32_t index) {
    for (uint32_t i = 0; i < agg->buffer_count; i++) {
        if (agg->buffers[i].index == index) {
            ready_chunk_t *last = &agg->buffers[agg->buffer_count - 1];
            if (i < agg->buffer_count - 1) {
                agg->buffers[i] = *last;
            }
            agg->buffer_count--;
            return;
        }
    }
}

void* download_sender_loop(void* arg) {
    download_aggregator_t *agg = (download_aggregator_t*)arg;
    int cfd = agg->cfd;
    int ok = 1;

    pthread_t current_thread = pthread_self(); 
    printf("[DL-SENDER-%lu] Loop started. Next expected index: %u\n", (unsigned long)current_thread, agg->next_index_to_send);

    //while there is still chunk
    while (agg->next_index_to_send < agg->total_chunks) {
        pthread_mutex_lock(&agg->lock);

        ready_chunk_t *rc = find_ready_chunk(agg, agg->next_index_to_send);

        while (rc == NULL && agg->has_error == 0 && agg->next_index_to_send < agg->total_chunks) {
            printf("[DL-SENDER-%lu] Waiting for Chunk Index: %u\n", (unsigned long)current_thread, agg->next_index_to_send);
            pthread_cond_wait(&agg->ready_cond, &agg->lock);
            rc = find_ready_chunk(agg, agg->next_index_to_send);
        }

        if (agg->has_error) {
            ok = 0;
            pthread_mutex_unlock(&agg->lock);
            break;
        }

        if (rc == NULL) { 
            pthread_mutex_unlock(&agg->lock);
            break;
        }

        pthread_mutex_unlock(&agg->lock); 
        
        if (send_frame(cfd, OP_DOWNLOAD_CHUNK, rc->data, rc->size) < 0) {
            ok = 0;
        }

        pthread_mutex_lock(&agg->lock);
        free(rc->data);
        remove_ready_chunk(agg, agg->next_index_to_send);
        printf("[DL-SENDER-%lu] SENT and CLEANED Chunk Index: %u\n", (unsigned long)current_thread, agg->next_index_to_send);
        agg->next_index_to_send++;
        pthread_mutex_unlock(&agg->lock);
        
        if (!ok) break; 
    }

    if (ok) {
        send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
    } else {
        send_error(cfd, "E_HASH_MISMATCH", "Download failed due to block error or I/O failure");
    }

    pthread_mutex_destroy(&agg->lock);
    pthread_cond_destroy(&agg->ready_cond);
    if (agg->manifest_chunks) free(agg->manifest_chunks);
    for(uint32_t i=0; i < agg->buffer_count; i++) {
        if (agg->buffers[i].data) free(agg->buffers[i].data);
    }
    if (agg->buffers) free(agg->buffers);
    
    return NULL;
}

void handle_download(int cfd, const uint8_t *payload, uint32_t len) {
    char cid_buf[HASH_HEX_LEN + 1];
    size_t cid_len = len;
    if (cid_len >= sizeof(cid_buf))
        cid_len = sizeof(cid_buf) - 1;
    memcpy(cid_buf, payload, cid_len);
    cid_buf[cid_len] = '\0';

    for (size_t i = 0; i < cid_len; i++) {
        char c = cid_buf[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            send_error(cfd, "E_BAD_CID", "Invalid CID");
            return;
        }
    }

    //building manifest path
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path),"manifests/%s.json", cid_buf);
    //check for manifest existence
    if (!file_exists(manifest_path)) {
        send_error(cfd, "E_NOT_FOUND", "Manifest not found");
        return;
    }
    // if it exists then read it into memory
    size_t mf_size = 0;
    uint8_t *mf_buf = read_file_into_buf(manifest_path, &mf_size);
    if (!mf_buf) {
        send_error(cfd, "E_IO", "Cannot read manifest");
        return;
    }

    chunk *chunks = NULL;
    int n_chunks = parse_manifest_chunks(mf_buf, mf_size, &chunks);
    free(mf_buf);
    if (n_chunks <= 0) {
        send_error(cfd, "E_PROTO", "Malformed manifest");
        return;
    }

    download_aggregator_t agg;
    memset(&agg, 0, sizeof(agg));
    agg.cfd = cfd;
    agg.total_chunks = (uint32_t)n_chunks;
    agg.manifest_chunks = chunks; 

    pthread_mutex_init(&agg.lock, NULL);
    pthread_cond_init(&agg.ready_cond, NULL);
    
    pthread_t sender_th;

    if (pthread_create(&sender_th, NULL, download_sender_loop, &agg) != 0) {
        send_error(cfd, "E_MEM", "Could not start sender thread");
        free(chunks);
        pthread_mutex_destroy(&agg.lock);
        pthread_cond_destroy(&agg.ready_cond);
        return;
    }
    printf("[DL-MAIN] Sender thread started. Dispatching %d chunk tasks.\n", n_chunks);

    for (int i = 0; i < n_chunks; i++) {
        chunk_task_arg_t *task_arg = malloc(sizeof(chunk_task_arg_t));
        if (!task_arg) {
            pthread_mutex_lock(&agg.lock);
            agg.has_error = 1;
            pthread_cond_broadcast(&agg.ready_cond);
            pthread_mutex_unlock(&agg.lock);
            break; 
        }
        task_arg->agg = &agg;
        task_arg->ci = &agg.manifest_chunks[i];
        thread_pool_add_task(&g_pool, process_chunk_task, task_arg);
        printf("[DL-MAIN] Dispatched task for Chunk Index: %u\n", agg.manifest_chunks[i].index);
    }


    pthread_join(sender_th, NULL);
}

static ready_chunk_t* find_ready_chunk(download_aggregator_t *agg, uint32_t index) {
    for (uint32_t i = 0; i < agg->buffer_count; i++) {
        if (agg->buffers[i].index == index) {
            return &agg->buffers[i];
        }
    }
    return NULL;
}

void aggregator_add_chunk(download_aggregator_t *agg, ready_chunk_t *rc) {
    pthread_mutex_lock(&agg->lock);

    if (agg->buffer_count == agg->buffer_cap) {
        agg->buffer_cap = agg->buffer_cap ? agg->buffer_cap * 2 : 16;
        agg->buffers = realloc(agg->buffers, agg->buffer_cap * sizeof(ready_chunk_t));
        if (!agg->buffers) {
            pthread_mutex_unlock(&agg->lock);
            return;
        }
    }

    // copping ready chunk to Aggregator
    agg->buffers[agg->buffer_count++] = *rc;
    
    pthread_cond_signal(&agg->ready_cond);
    
    pthread_mutex_unlock(&agg->lock);
}

void process_chunk_task(void* arg) {
    chunk_task_arg_t *task_arg = (chunk_task_arg_t*)arg;
    download_aggregator_t *agg = task_arg->agg;
    const chunk *ci = task_arg->ci;
    pthread_t current_thread = pthread_self();
    printf("[DL-WORKER-%lu] Starting work on Chunk Index: %u\n", (unsigned long)current_thread, ci->index);

    char dir1[64], dir2[128], filepath[512];
    snprintf(dir1, sizeof(dir1), "blocks/%c%c", ci->hash[0], ci->hash[1]);
    snprintf(dir2, sizeof(dir2), "%s/%c%c", dir1, ci->hash[2], ci->hash[3]);
    snprintf(filepath, sizeof(filepath), "%s/%s.chunk", dir2, ci->hash);

    uint8_t *all = malloc(ci->size);
    int success = 0;
    
    if (all) {
        FILE *f = fopen(filepath, "rb");
        size_t read_total = 0;
        
        if (f) {
            read_total = fread(all, 1, ci->size, f);
            fclose(f);

            char computed_hash[HASH_HEX_LEN + 1];
            if (read_total == ci->size) {
                chunk_hash_hex(all, read_total, computed_hash);
                if (strncmp(computed_hash, ci->hash, HASH_HEX_LEN) == 0) 
                    success = 1;    
            }
        }
    }
    
    if (success) {
        ready_chunk_t rc = {.index = ci->index, .size = ci->size, .data = all};
        aggregator_add_chunk(agg, &rc);
        printf("[DL-WORKER-%lu] Finished and Added READY Chunk Index: %u\n", (unsigned long)current_thread, ci->index);
    } 
    else {
        pthread_mutex_lock(&agg->lock);
        agg->has_error = 1;
        pthread_cond_broadcast(&agg->ready_cond);
        pthread_mutex_unlock(&agg->lock);
        if (all) free(all);
        printf("[DL-WORKER-%lu] ERROR or Hash mismatch for Chunk Index: %u\n", (unsigned long)current_thread, ci->index);
    }
    free(task_arg);
}

