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

// New: raw BLAKE3-256 hash (32 bytes, no hex)
void blake3_raw_hash(const uint8_t *data, size_t len, uint8_t out[MH_BLAKE3_256_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, out, MH_BLAKE3_256_SIZE);
}

// New: multihash(blake3-256, data) => [code, length, digest...]
size_t make_multihash_blake3_256(const uint8_t *data, size_t len,
                                 uint8_t *out, size_t out_cap) {
    // We need at least 1 byte code + 1 byte length + 32 bytes digest
    if (out_cap < 2 + MH_BLAKE3_256_SIZE) {
        return 0;
    }

    uint8_t digest[MH_BLAKE3_256_SIZE];
    blake3_raw_hash(data, len, digest);

    size_t pos = 0;
    out[pos++] = MH_BLAKE3_256;         // hash function code
    out[pos++] = MH_BLAKE3_256_SIZE;    // digest length
    memcpy(&out[pos], digest, MH_BLAKE3_256_SIZE);
    pos += MH_BLAKE3_256_SIZE;

    return pos;
}

// New: CID bytes for a manifest: [CID_CODEC_MANIFEST | multihash...]
size_t make_cid_bytes_for_manifest(const uint8_t *manifest,
                                   size_t manifest_len,
                                   uint8_t *out, size_t out_cap) {
    // We need at least 1 byte codec + multihash length
    if (out_cap < 1 + 2 + MH_BLAKE3_256_SIZE) {
        return 0;
    }

    size_t pos = 0;
    out[pos++] = CID_CODEC_MANIFEST;

    size_t mh_len = make_multihash_blake3_256(manifest, manifest_len,
                                              &out[pos], out_cap - pos);
    if (mh_len == 0) {
        return 0;
    }
    pos += mh_len;

    return pos;
}

// New: Base32 encoder (RFC 4648, no padding)
static const char BASE32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t base32_encode(const uint8_t *data, size_t len,
                     char *out, size_t out_cap) {
    uint32_t buffer = 0;
    int bits_in_buffer = 0;
    size_t out_len = 0;

    for (size_t i = 0; i < len; i++) {
        buffer = (buffer << 8) | data[i];
        bits_in_buffer += 8;

        while (bits_in_buffer >= 5) {
            bits_in_buffer -= 5;
            uint8_t index = (buffer >> bits_in_buffer) & 0x1F;
            if (out_len >= out_cap) {
                return 0; // output buffer too small
            }
            out[out_len++] = BASE32_ALPHABET[index];
        }
    }

    if (bits_in_buffer > 0) {
        buffer <<= (5 - bits_in_buffer);
        uint8_t index = buffer & 0x1F;
        if (out_len >= out_cap) {
            return 0;
        }
        out[out_len++] = BASE32_ALPHABET[index];
    }

    if (out_len >= out_cap) {
        return 0;
    }
    out[out_len] = '\0';
    return out_len;
}

static void* worker_loop(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg;
    task_t* task = NULL;

    for (;;) {
        pthread_mutex_lock(&pool->queue.lock);

        while (pool->queue.head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->queue.notify, &pool->queue.lock);
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