#include "upload.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>   // for fsync
#include <fcntl.h>

#define OP_UPLOAD_DONE   0x81

extern thread_pool_t g_pool;

extern int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);
extern void send_error(int fd, const char *code, const char *message);

// Worker thread function
void process_upload_chunk_task(void* arg) {
    upload_task_arg_t *task_arg = (upload_task_arg_t*)arg;
    upload_S *sess = task_arg->session;
    pthread_t current_thread = pthread_self();
    int ok = 0;

    printf("[UP-WORKER-%lu] Starting work on Chunk Index: %u\n",
           (unsigned long)current_thread, task_arg->chunk_index);

    char hash_hex[HASH_HEX_LEN + 1];
    chunk_hash_hex(task_arg->data, task_arg->chunk_len, hash_hex);
    memcpy(task_arg->hash_hex, hash_hex, HASH_HEX_LEN + 1);

    char dir1[64], dir2[128], filepath[256];

    ensure_dir("blocks");

    snprintf(dir1,sizeof(dir1),"blocks/%c%c",task_arg->hash_hex[0],task_arg->hash_hex[1]);
    ensure_dir(dir1);

    snprintf(dir2,sizeof(dir2),"%s/%c%c",dir1,task_arg->hash_hex[2],task_arg->hash_hex[3]);
    ensure_dir(dir2);

    snprintf(filepath,sizeof(filepath),"%s/%s.chunk",dir2,task_arg->hash_hex);

    char refpath[256];
    snprintf(refpath, sizeof(refpath), "%s/%s.ref", dir2, task_arg->hash_hex);

    if (file_exists(filepath)) {
        if (block_ref_increment(refpath) == 0)
            ok = 1;
        else
            ok = 0;
    }
    else{
        FILE *fp=fopen(filepath,"wb");
        if(fp){
            if(fwrite(task_arg->data, 1, task_arg->chunk_len, fp) == task_arg->chunk_len)
                ok = 1;

            fclose(fp);
        }

        if (ok) {
            if (block_ref_increment(refpath) != 0)
                perror("block_ref_increment (new block)");
        }
    }

    pthread_mutex_lock(&sess->lock);

    if (ok) {
        if(sess->chunk_count == sess->chunk_cap){
            if(sess->chunk_cap > 0 ) sess->chunk_cap = (sess->chunk_cap * 2);
            else sess->chunk_cap = 16;

            chunk *new_chunks = realloc(sess->chunks, sizeof(chunk) * sess->chunk_cap);
            if(new_chunks) sess->chunks = new_chunks;
            else ok = 0; // reallocation failed
        }

        if (ok) {
            uint32_t idx = sess->chunk_count++;

            sess->chunks[idx].index = task_arg->chunk_index;
            sess->chunks[idx].size  = task_arg->chunk_len;
            memcpy(sess->chunks[idx].hash, task_arg->hash_hex, HASH_HEX_LEN + 1);

            sess->total_size += task_arg->chunk_len;

            printf("[UP-WORKER-%lu] Finished storing Chunk Index: %u\n",
                   (unsigned long)current_thread, task_arg->chunk_index);
        }
    }
    else {
        printf("[UP-WORKER-%lu] ERROR storing Chunk Index: %u\n",
               (unsigned long)current_thread, task_arg->chunk_index);
    }

    sess->tasks_in_progress--;

    printf("[UP-WORKER-%lu] Tasks Left: %u\n",
           (unsigned long)current_thread, sess->tasks_in_progress);

    if (sess->tasks_in_progress == 0) {
        pthread_cond_signal(&sess->finished_cond); // going to handle_upload_finish
    }

    pthread_mutex_unlock(&sess->lock);

    free(task_arg->data);
    free(task_arg);
}

void handle_upload_start(int cfd, upload_S *sess, const uint8_t *payload, uint32_t len){

    memset(sess, 0, sizeof(*sess));

    size_t fn_len = len;
    if(fn_len >= sizeof(sess->filename))
        fn_len = sizeof(sess->filename) - 1;

    memcpy(sess->filename, payload,fn_len);
    sess->filename[fn_len] = '\0';

    // Default chunk size must match the Python gateway's default
    uint32_t chunk_size = 256 * 1024;

    // Read CHUNK_SIZE from environment (bytes), if present
    const char *env_chunk = getenv("CHUNK_SIZE");
    if (env_chunk && env_chunk[0] != '\0') {
        char *endptr = NULL;
        long val = strtol(env_chunk, &endptr, 10);
        if (endptr != env_chunk && *endptr == '\0' && val > 0) {
            // Optional: clamp to a reasonable range, e.g. [1 KB, 16 MB]
            if (val < 1024) val = 1024;
            if (val > 16 * 1024 * 1024) val = 16 * 1024 * 1024;
            chunk_size = (uint32_t)val;
        } else {
            printf("[ENGINE] WARNING: invalid CHUNK_SIZE='%s', using default %u\n",
                   env_chunk, chunk_size);
        }
    }

    sess->chunk_size = chunk_size;

    // Hash algorithm (we still only support blake3)
    strncpy(sess->hash_algo, "blake3", sizeof(sess->hash_algo) - 1);
    sess->hash_algo[sizeof(sess->hash_algo) - 1] = '\0';

    snprintf(sess->temp_manifest_path,sizeof(sess->temp_manifest_path),
             "manifests/%s.tmp",sess->filename);

    pthread_mutex_init(&sess->lock, NULL);
    pthread_cond_init(&sess->finished_cond, NULL);
    sess->tasks_in_progress = 0;
    sess->next_index = 0;

    printf("[ENGINE] UPLOAD_START: %s (CHUNK_SIZE=%u)\n",
           sess->filename, sess->chunk_size);
}

void handle_upload_chunk(int cfd,upload_S *sess,const uint8_t *payload,uint32_t len){

    if(sess->filename[0]=='\0'){
        send_error(cfd,"E_PROTO","UPLOAD_CHUNK before UPLOAD_START");
        return;
    }

    upload_task_arg_t *task_arg = malloc(sizeof(upload_task_arg_t));
    if (!task_arg) {
        send_error(cfd, "E_MEM", "Out of memory for task arg");
        if (sess->chunks) free(sess->chunks);
        sess->chunks = NULL;
        return;
    }
    task_arg->data = malloc(len);
    if (!task_arg->data) {
        free(task_arg);
        send_error(cfd, "E_MEM", "Out of memory for chunk data");
        if (sess->chunks) free(sess->chunks);
        sess->chunks = NULL;
        return;
    }
    memcpy(task_arg->data, payload, len);
    task_arg->chunk_len = len;
    task_arg->session = sess;

    pthread_mutex_lock(&sess->lock);
    sess->tasks_in_progress++;
    task_arg->chunk_index = sess->next_index++;
    pthread_mutex_unlock(&sess->lock);

    thread_pool_add_task(&g_pool, process_upload_chunk_task, task_arg);
}

void handle_upload_finish(int cfd,upload_S *sess){
    pthread_mutex_lock(&sess->lock);

    printf("[UP-FINISH] Waiting for %u tasks to complete...\n", sess->tasks_in_progress);

    // Wait while there is any unfinished task
    while (sess->tasks_in_progress > 0)
        pthread_cond_wait(&sess->finished_cond, &sess->lock);

    pthread_mutex_unlock(&sess->lock);

    // Acquire write lock for manifest operations
    pthread_rwlock_wrlock(&g_manifest_rwlock);

    ensure_dir("manifests");

    FILE *mf = fopen(sess->temp_manifest_path,"w");
    if (!mf) {
        perror("fopen manifest temp");
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_IO", "Cannot open manifest temp file");
        return;
    }

    fprintf(mf, "{ \"version\":1,\"hash_algo\":\"%s\",\"chunk_size\":%u,"
                "\"total_size\":%llu,\"filename\":\"%s\",\"chunks\":[",
            sess->hash_algo,sess->chunk_size,
            (unsigned long long)sess->total_size,sess->filename);

    for(uint32_t i = 0; i < sess->chunk_count; i++){
        chunk *ci = &sess->chunks[i];
        fprintf(mf,"%s{\"index\":%u,\"size\":%u,\"hash\":\"%s\"}",
                (i?",":""),ci->index,ci->size,ci->hash);
    }
    fprintf(mf,"]}");

    // Flush stdio buffer
    if (fflush(mf) != 0) {
        perror("fflush manifest temp");
        fclose(mf);
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_IO", "Cannot flush manifest temp file");
        return;
    }

    // Ensure data is on disk before rename
    int fd = fileno(mf);
    if (fd == -1) {
        perror("fileno manifest temp");
        fclose(mf);
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_IO", "Cannot get fileno for manifest temp file");
        return;
    }
    if (fsync(fd) != 0) {
        perror("fsync manifest temp");
        fclose(mf);
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_IO", "Cannot fsync manifest temp file");
        return;
    }

    fclose(mf);

    // Read manifest back into memory to compute CID
    size_t mf_size = 0;
    uint8_t *buf = read_file_into_buf(sess->temp_manifest_path,&mf_size);
    if (!buf) {
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_IO", "Cannot read manifest temp");
        return;
    }

    // Build raw CID bytes = [CID_CODEC_MANIFEST | multihash(blake3-256, manifest)]
    uint8_t cid_raw[64]; // enough for 1 + 2 + 32 = 35 bytes
    size_t cid_raw_len = make_cid_bytes_for_manifest(buf, mf_size,
                                                     cid_raw, sizeof(cid_raw));
    free(buf);
    if (cid_raw_len == 0) {
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_INTERNAL", "CID generation failed");
        return;
    }

    // Base32 encode the CID bytes
    char cid_str[CID_MAX_LEN + 1];
    size_t cid_str_len = base32_encode(cid_raw, cid_raw_len,
                                       cid_str, sizeof(cid_str));
    if (cid_str_len == 0) {
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_INTERNAL", "CID base32 encode failed");
        return;
    }

    // Store CID in session
    if (cid_str_len >= sizeof(sess->cid)) {
        pthread_rwlock_unlock(&g_manifest_rwlock);
        send_error(cfd, "E_INTERNAL", "CID too long for buffer");
        return;
    }
    memcpy(sess->cid, cid_str, cid_str_len + 1);

    // Final manifest path uses CID string
    char final_path[512];
    snprintf(final_path,sizeof(final_path),"manifests/%s.json",cid_str);
    rename(sess->temp_manifest_path,final_path);

    // Release write lock
    pthread_rwlock_unlock(&g_manifest_rwlock);

    // Send CID back to gateway
    send_frame(cfd,OP_UPLOAD_DONE,cid_str,(uint32_t)cid_str_len);

    printf("[ENGINE] UPLOAD_FINISH -> CID=%s\n",cid_str);

    pthread_mutex_destroy(&sess->lock);
    pthread_cond_destroy(&sess->finished_cond);
    if(sess->chunks) free(sess->chunks);
    memset(sess,0,sizeof(*sess));
}