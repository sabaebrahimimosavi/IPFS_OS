#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include "blake3.h"

void ensure_dir(const char *path) {
    if (mkdir(path, 0777) < 0 && errno != EEXIST) {
        perror("mkdir");
    }
}

bool file_exists(const char *path){
    struct stat st;
    return stat(path, &st)==0;
}

int block_ref_increment(const char *refpath) {
    unsigned long long val = 0;

    FILE *f = fopen(refpath, "r");
    if (f) {
        if (fscanf(f, "%llu", &val) != 1) {
            val = 0; 
        }
        fclose(f);
    }

    val++;
    f = fopen(refpath, "w");
    if (!f) {
        perror("block_ref_increment: fopen");
        return -1;
    }
    fprintf(f, "%llu\n", val);
    fclose(f);
    return 0;
}

uint8_t *read_file_into_buf(const char* path , size_t *output_size){
    FILE *rf = fopen(path, "rb");
    if (!rf) return NULL;

    fseek(rf, 0, SEEK_END);
    long s = ftell(rf);
    rewind(rf);

    if (s <= 0) { fclose(rf); return NULL; }

    uint8_t *buf = malloc(s+1);
    if (!buf) { fclose(rf); return NULL; }

    size_t rd = fread(buf,1,(size_t)s,rf);
    fclose(rf);

    if (rd != (size_t)s) { free(buf); return NULL; }

    buf[rd] = '\0';
    if (output_size) *output_size = rd;
    return buf;
}

void chunk_hash_hex(const uint8_t *data, size_t len,char out_hex[HASH_HEX_LEN + 1]) {
    // BLAKE3 output is 32 bytes
    uint8_t digest[32];
    blake3_hasher hasher;

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));

    static const char hexdigit[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        out_hex[i*2]     = hexdigit[digest[i] >> 4];
        out_hex[i*2 + 1] = hexdigit[digest[i] & 0x0F];
    }

    out_hex[64] = '\0';
}

static void* worker_loop(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg;
    task_t* task = NULL;

    for (;;) {
        pthread_mutex_lock(&pool->queue.lock);

        // Set timeout of 5 seconds
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;  // 5 second timeout

        // Either a job or timeout
        while (pool->queue. head == NULL && !pool->shutdown) {
            int ret = pthread_cond_timedwait(&pool->queue.notify, &pool->queue.lock, &timeout);
            if (ret == ETIMEDOUT) {
                // Timeout occurred - check if we should exit
                if (pool->queue.head == NULL) {
                    pthread_mutex_unlock(&pool->queue.lock);
                    return NULL;  // Exit worker thread on timeout
                }
            }
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->queue.lock);
            break;
        }

        task = pool->queue.head;
        pool->queue.head = task->next;
        if (pool->queue.head == NULL)
            pool->queue.tail = NULL;

        pthread_mutex_unlock(&pool->queue.lock);

        if (task != NULL) {
            task->func(task->arg);
            free(task);
        }
    }
    return NULL;
}

void thread_pool_init(thread_pool_t* pool, int num_threads) {
    pool->num_threads = num_threads;
    pool->shutdown = 0;
    pool->threads = malloc(sizeof(pthread_t) * num_threads);

    pthread_mutex_init(&pool->queue.lock, NULL);
    pthread_cond_init(&pool->queue.notify, NULL);
    pool->queue.head = pool->queue.tail = NULL;

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_loop, pool);
    }
}

void thread_pool_add_task(thread_pool_t* pool, void (*func)(void*), void* arg) {
    task_t* new_task = malloc(sizeof(task_t));
    new_task->func = func;
    new_task->arg = arg;
    new_task->next = NULL;

    pthread_mutex_lock(&pool->queue.lock);

    if (pool->queue.tail == NULL) {
        pool->queue.head = new_task;
        pool->queue.tail = new_task;
    } 
    else {
        pool->queue.tail->next = new_task;
        pool->queue.tail = new_task;
    }

    pthread_cond_signal(&pool->queue.notify);
    pthread_mutex_unlock(&pool->queue.lock);
}

void thread_pool_destroy(thread_pool_t* pool) {
    pthread_mutex_lock(&pool->queue.lock);
    pool->shutdown = 1;
    pthread_mutex_unlock(&pool->queue.lock);

    pthread_cond_broadcast(&pool->queue.notify);

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    pthread_mutex_destroy(&pool->queue.lock);
    pthread_cond_destroy(&pool->queue.notify);

}

