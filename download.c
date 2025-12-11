#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "download.h"
#include "common.h"
#include "blake3.h"

extern void send_error(int fd, const char *code, const char *message);
extern int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);
extern thread_pool_t g_pool;

#define DOWNLOAD_BUF_SZ   (256 * 1024)
#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

// Forward declarations
static ready_chunk_t* find_ready_chunk(download_aggregator_t *agg, uint32_t index);
static void aggregator_add_chunk(download_aggregator_t *agg, ready_chunk_t *rc);
static void* download_sender_loop(void* arg);
static void process_chunk_task(void* arg);

// ---------------------- Manifest Parsing ----------------------

int parse_manifest_chunks(const uint8_t *buf , size_t buf_length , chunk **output_chunks){
    const char *s = (const char*)buf;

    const char *chunks_pos = strstr(s, "\"chunks\"");
    if (!chunks_pos) return -1;

    const char *arr_start = strchr(chunks_pos, '[');
    if (!arr_start) return -1;
    const char *p = arr_start + 1;

    uint32_t cap = 16;
    uint32_t count = 0;
    chunk *arr = malloc(cap * sizeof(chunk));
    if (!arr) return -1;

    while (1){
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) { free(arr); return -1; }

        const char *idx_pos  = strstr(obj, "\"index\"");
        const char *size_pos = strstr(obj, "\"size\"");
        const char *hash_pos = strstr(obj, "\"hash\"");
        if (!idx_pos || !size_pos || !hash_pos) {
            free(arr);
            return -1;
        }

        unsigned long idx_val = 0, size_val = 0;

        if (sscanf(idx_pos, "\"index\"%*[^0-9]%lu", &idx_val) != 1) {
            free(arr);
            return -1;
        }

        if (sscanf(size_pos, "\"size\"%*[^0-9]%lu", &size_val) != 1) {
            free(arr);
            return -1;
        }

        const char *q = strchr(hash_pos, '"');
        if (!q) { free(arr); return -1; }
        q = strchr(q + 1, '"');
        if (!q) { free(arr); return -1; }

        const char *val_start = strchr(q + 1, '"');
        if (!val_start) { free(arr); return -1; }
        val_start++;

        const char *val_end = strchr(val_start, '"');
        if (!val_end) { free(arr); return -1; }

        size_t h_len = (size_t)(val_end - val_start);
        if (h_len > HASH_HEX_LEN) h_len = HASH_HEX_LEN;

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

        count++;
        p = obj_end + 1;
    }

    *output_chunks = arr;
    return (int) count;
}

// ---------------------- Aggregator Helpers ----------------------

static ready_chunk_t* find_ready_chunk(download_aggregator_t *agg, uint32_t index) {
    for (uint32_t i = 0; i < agg->buffer_count; i++) {
        if (agg->buffers[i].index == index) {
            return &agg->buffers[i];
        }
    }
    return NULL;
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

static void aggregator_add_chunk(download_aggregator_t *agg, ready_chunk_t *rc) {
    pthread_mutex_lock(&agg->lock);

    if (agg->has_error) {
        // We no longer need this chunk.
        pthread_mutex_unlock(&agg->lock);
        if (rc->data) free(rc->data);
        return;
    }

    if (agg->buffer_count == agg->buffer_cap) {
        uint32_t new_cap = agg->buffer_cap ? agg->buffer_cap * 2 : 16;
        ready_chunk_t *new_buf =
                realloc(agg->buffers, new_cap * sizeof(ready_chunk_t));
        if (!new_buf) {
            // Memory failure: mark error so sender can abort.
            agg->has_error = 1;
            pthread_cond_broadcast(&agg->ready_cond);
            pthread_mutex_unlock(&agg->lock);
            if (rc->data) free(rc->data);
            return;
        }
        agg->buffers = new_buf;
        agg->buffer_cap = new_cap;
    }

    agg->buffers[agg->buffer_count++] = *rc;
    pthread_cond_broadcast(&agg->ready_cond);
    pthread_mutex_unlock(&agg->lock);
}

// ---------------------- Worker Task ----------------------

static void process_chunk_task(void* arg) {
    chunk_task_arg_t *task_arg = (chunk_task_arg_t*)arg;
    download_aggregator_t *agg = task_arg->agg;
    const chunk *ci = task_arg->ci;

    pthread_t current_thread = pthread_self();
    printf("[DL-WORKER-%lu] Starting work on Chunk Index: %u\n",
           (unsigned long)current_thread, ci->index);

    // Build file path: blocks/aa/bb/hash.chunk
    char dir1[64], dir2[128], filepath[512];
    snprintf(dir1, sizeof(dir1), "blocks/%c%c", ci->hash[0], ci->hash[1]);
    snprintf(dir2, sizeof(dir2), "%s/%c%c", dir1, ci->hash[2], ci->hash[3]);
    snprintf(filepath, sizeof(filepath), "%s/%s.chunk", dir2, ci->hash);

    uint8_t *buf = malloc(ci->size);
    int success = 0;

    if (buf) {
        FILE *f = fopen(filepath, "rb");
        if (f) {
            size_t read_total = fread(buf, 1, ci->size, f);
            fclose(f);

            if (read_total == ci->size) {
                char computed_hash[HASH_HEX_LEN + 1];
                chunk_hash_hex(buf, read_total, computed_hash);
                if (strncmp(computed_hash, ci->hash, HASH_HEX_LEN) == 0) {
                    success = 1;
                }
            }
        }
    }

    if (success) {
        ready_chunk_t rc;
        rc.index = ci->index;
        rc.size  = ci->size;
        rc.data  = buf;

        aggregator_add_chunk(agg, &rc);
        printf("[DL-WORKER-%lu] Finished and Added READY Chunk Index: %u\n",
               (unsigned long)current_thread, ci->index);
    } else {
        pthread_mutex_lock(&agg->lock);
        agg->has_error = 1;
        pthread_cond_broadcast(&agg->ready_cond);
        pthread_mutex_unlock(&agg->lock);
        if (buf) free(buf);
        printf("[DL-WORKER-%lu] ERROR or Hash mismatch for Chunk Index: %u\n",
               (unsigned long)current_thread, ci->index);
    }

    free(task_arg);
}

// ---------------------- Sender Thread ----------------------

static void* download_sender_loop(void* arg) {
    download_aggregator_t *agg = (download_aggregator_t*)arg;
    int cfd = agg->cfd;
    int ok = 1;

    pthread_t current_thread = pthread_self();
    printf("[DL-SENDER-%lu] Loop started. Next expected index: %u, total=%u\n",
           (unsigned long)current_thread, agg->next_index_to_send, agg->total_chunks);

    while (agg->next_index_to_send < agg->total_chunks) {
        pthread_mutex_lock(&agg->lock);

        uint32_t idx = agg->next_index_to_send;
        ready_chunk_t *rc = find_ready_chunk(agg, idx);

        while (rc == NULL && !agg->has_error && idx < agg->total_chunks) {
            printf("[DL-SENDER-%lu] Waiting for Chunk Index: %u\n",
                   (unsigned long)current_thread, idx);
            pthread_cond_wait(&agg->ready_cond, &agg->lock);
            rc = find_ready_chunk(agg, idx);
        }

        if (agg->has_error) {
            printf("[DL-SENDER-%lu] Detected has_error=1, aborting\n",
                   (unsigned long)current_thread);
            ok = 0;
            pthread_mutex_unlock(&agg->lock);
            break;
        }

        if (rc == NULL) {
            printf("[DL-SENDER-%lu] rc == NULL for index=%u, breaking loop\n",
                   (unsigned long)current_thread, idx);
            pthread_mutex_unlock(&agg->lock);
            break;
        }

        printf("[DL-SENDER-%lu] Found ready chunk index=%u size=%u\n",
               (unsigned long)current_thread, rc->index, rc->size);

        uint8_t *chunk_data = malloc(rc->size);
        if (!chunk_data) {
            printf("[DL-SENDER-%lu] malloc failed for chunk_data\n",
                   (unsigned long)current_thread);
            ok = 0;
            pthread_mutex_unlock(&agg->lock);
            break;
        }
        memcpy(chunk_data, rc->data, rc->size);
        uint32_t chunk_size = rc->size;
        uint32_t current_index = rc->index;

        free(rc->data);
        rc->data = NULL;
        remove_ready_chunk(agg, current_index);
        agg->next_index_to_send++;

        printf("[DL-SENDER-%lu] Prepared chunk index=%u, next_index_to_send now=%u\n",
               (unsigned long)current_thread, current_index, agg->next_index_to_send);

        pthread_mutex_unlock(&agg->lock);

        printf("[DL-SENDER-%lu] Sending OP_DOWNLOAD_CHUNK for index=%u\n",
               (unsigned long)current_thread, current_index);
        if (send_frame(cfd, OP_DOWNLOAD_CHUNK, chunk_data, chunk_size) < 0) {
            printf("[DL-SENDER-%lu] send_frame(OP_DOWNLOAD_CHUNK) failed\n",
                   (unsigned long)current_thread);
            ok = 0;
            free(chunk_data);
            break;
        }

        free(chunk_data);
    }

    if (ok) {
        printf("[DL-SENDER-%lu] All chunks sent, sending OP_DOWNLOAD_DONE\n",
               (unsigned long)current_thread);
        send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
        printf("[ENGINE] UPLOAD_DONE\n");
        printf("[DL-SENDER-%lu] Download completed successfully\n",
               (unsigned long)current_thread);
    } else {
        printf("[DL-SENDER-%lu] Download failed, sending ERROR\n",
               (unsigned long)current_thread);
        send_error(cfd, "E_HASH_MISMATCH",
                   "Download failed due to block error or I/O failure");
    }

    // cleanup as before...
    pthread_mutex_destroy(&agg->lock);
    pthread_cond_destroy(&agg->ready_cond);

    if (agg->manifest_chunks) {
        free(agg->manifest_chunks);
    }
    for (uint32_t i = 0; i < agg->buffer_count; i++) {
        if (agg->buffers[i].data) {
            free(agg->buffers[i].data);
        }
    }
    free(agg->buffers);
    free(agg);

    printf("[DL-SENDER-%lu] Sender thread exiting\n",
           (unsigned long)current_thread);
    return NULL;
}

// ---------------------- Public Entry: handle_download ----------------------

void handle_download(int cfd, const uint8_t *payload, uint32_t len) {
    printf("[DL] handle_download: len=%u\n", len);

    char cid_buf[HASH_HEX_LEN + 1];
    size_t cid_len = len;
    if (cid_len >= sizeof(cid_buf))
        cid_len = sizeof(cid_buf) - 1;
    memcpy(cid_buf, payload, cid_len);
    cid_buf[cid_len] = '\0';
    printf("[DL] CID='%s'\n", cid_buf);

    // validate hex
    for (size_t i = 0; i < cid_len; i++) {
        char c = cid_buf[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            printf("[DL] BAD CID char at %zu\n", i);
            send_error(cfd, "E_BAD_CID", "Invalid CID");
            return;
        }
    }

    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "manifests/%s.json", cid_buf);
    printf("[DL] manifest_path=%s\n", manifest_path);

    if (!file_exists(manifest_path)) {
        printf("[DL] manifest not found\n");
        send_error(cfd, "E_NOT_FOUND", "Manifest not found");
        return;
    }

    size_t mf_size = 0;
    uint8_t *mf_buf = read_file_into_buf(manifest_path, &mf_size);
    if (!mf_buf) {
        printf("[DL] read_file_into_buf failed\n");
        send_error(cfd, "E_IO", "Cannot read manifest");
        return;
    }

    printf("[DL] manifest read, size=%zu\n", mf_size);

    chunk *chunks = NULL;
    int n_chunks = parse_manifest_chunks(mf_buf, mf_size, &chunks);
    free(mf_buf);
    printf("[DL] parse_manifest_chunks n_chunks=%d\n", n_chunks);

    if (n_chunks <= 0) {
        send_error(cfd, "E_PROTO", "Malformed manifest");
        return;
    }

    download_aggregator_t *agg = malloc(sizeof(download_aggregator_t));
    if (!agg) {
        send_error(cfd, "E_MEM", "Cannot allocate aggregator");
        free(chunks);
        return;
    }

    memset(agg, 0, sizeof(*agg));
    agg->cfd = cfd;
    agg->total_chunks = (uint32_t)n_chunks;
    agg->manifest_chunks = chunks;
    printf("[DL] total_chunks=%u\n", agg->total_chunks);

    pthread_mutex_init(&agg->lock, NULL);
    pthread_cond_init(&agg->ready_cond, NULL);

    pthread_t sender_th;
    if (pthread_create(&sender_th, NULL, download_sender_loop, agg) != 0) {
        printf("[DL] pthread_create sender failed\n");
        send_error(cfd, "E_MEM", "Could not start sender thread");
        free(chunks);
        pthread_mutex_destroy(&agg->lock);
        pthread_cond_destroy(&agg->ready_cond);
        free(agg);
        return;
    }

    printf("[DL-MAIN] Sender thread started. Dispatching %d chunk tasks.\n", n_chunks);

    for (int i = 0; i < n_chunks; i++) {
        chunk_task_arg_t *task_arg = malloc(sizeof(chunk_task_arg_t));
        if (!task_arg) {
            printf("[DL-MAIN] malloc task_arg failed at i=%d\n", i);
            pthread_mutex_lock(&agg->lock);
            agg->has_error = 1;
            pthread_cond_broadcast(&agg->ready_cond);
            pthread_mutex_unlock(&agg->lock);
            break;
        }
        task_arg->agg = agg;
        task_arg->ci = &agg->manifest_chunks[i];
        printf("[DL-MAIN] Dispatched task for Chunk Index: %u\n", agg->manifest_chunks[i].index);
        thread_pool_add_task(&g_pool, process_chunk_task, task_arg);
    }

    pthread_join(sender_th, NULL);
    printf("[DL] handle_download done (sender thread joined)\n");
}