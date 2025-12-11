#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>

// BLAKE3 = 32 bytes = 64 hex chars for chunk hashes
#define HASH_HEX_LEN 64

// CID format constants for multicodec + multihash(blake3-256)
// NOTE: These codes are example values for this project.
// They do not need to match official IPFS codes as long as you are consistent.
#define CID_CODEC_MANIFEST       0x71   // Example multicodec code for "manifest"
#define MH_BLAKE3_256            0x1f   // Example multihash code for blake3-256
#define MH_BLAKE3_256_SIZE       32     // 32-byte BLAKE3-256 output

// Raw CID bytes: 1 byte codec + 1 byte hash code + 1 byte length + 32 bytes digest = 35 bytes.
// Base32 expands size, so allocate a safely large buffer.
#define CID_MAX_LEN              80

// task
typedef struct task {
    void (*func)(void*); // function
    void* arg;
    struct task* next; // pointer to next task
} task_t;

// Job queue
typedef struct {
    task_t* head;
    task_t* tail;
    pthread_mutex_t lock;
    pthread_cond_t notify; // to wake workers when there is a new job
} job_queue_t;

// Thread pool
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

// Raw BLAKE3 hash without hex encoding
void blake3_raw_hash(const uint8_t *data, size_t len, uint8_t out[MH_BLAKE3_256_SIZE]);

// Build multihash(blake3-256, data)
size_t make_multihash_blake3_256(const uint8_t *data, size_t len,
                                 uint8_t *out, size_t out_cap);

// Build raw CID bytes for a manifest: [CID_CODEC_MANIFEST | multihash...]
size_t make_cid_bytes_for_manifest(const uint8_t *manifest,
                                   size_t manifest_len,
                                   uint8_t *out, size_t out_cap);

// Base32 encoder (RFC 4648, no padding)
size_t base32_encode(const uint8_t *data, size_t len,
                     char *out, size_t out_cap);

// Global read/write lock for manifest operations
extern pthread_rwlock_t g_manifest_rwlock;

// Init/destroy helpers for manifest RW lock
void init_manifest_lock(void);
void destroy_manifest_lock(void);

// Thread Pool
void thread_pool_init(thread_pool_t* pool, int num_threads);
void thread_pool_add_task(thread_pool_t* pool, void (*func)(void*), void* arg);
void thread_pool_destroy(thread_pool_t* pool);

#endif