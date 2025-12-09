#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>

// BLAKE3 = 32 bytes = 64 hex chars
#define HASH_HEX_LEN 64

// task
typedef struct task {
    void (*func)(void*); //function
    void* arg;    
    struct task* next; // pointer to next task
} task_t;

//Job Queueu
typedef struct {
    task_t* head;
    task_t* tail;
    pthread_mutex_t lock;
    pthread_cond_t notify; //to find workers when there is a new job
} job_queue_t;

//Thread pool
typedef struct {
    pthread_t* threads;
    int num_threads;
    job_queue_t queue;
    int shutdown;
} thread_pool_t;

void ensure_dir(const char *path);
bool file_exists(const char *path);
int block_ref_increment(const char *refpath);
uint8_t *read_file_into_buf(const char* path , size_t *output_size);
void chunk_hash_hex(const uint8_t *data, size_t len, char out_hex[HASH_HEX_LEN + 1]);

// Thread Pool
void thread_pool_init(thread_pool_t* pool, int num_threads);
void thread_pool_add_task(thread_pool_t* pool, void (*func)(void*), void* arg);
void thread_pool_destroy(thread_pool_t* pool);

#endif
