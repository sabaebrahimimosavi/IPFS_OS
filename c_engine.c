// Build: gcc -O2 -pthread -o c_engine c_engine.c
// Run:   ./c_engine /tmp/cengine.sock

#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//
#include <sys/stat.h>   // for mkdir

#define OP_UPLOAD_START  0x01
#define OP_UPLOAD_CHUNK  0x02
#define OP_UPLOAD_FINISH 0x03
#define OP_UPLOAD_DONE   0x81

#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

#define OP_ERROR 0xFF
#define HASH_HEX_LEN 16   // function output 32 byte hash = 64 character


static const char* g_sock_path = NULL;

typedef struct{
        uint32_t index;
        uint32_t size;
        char hash[HASH_HEX_LEN + 1];  // hash (HEX)
}chunk;


typedef struct {
    char filename[256];
    uint64_t total_size;
    uint32_t chunk_size;
    char hash_algo[32];
    char temp_manifest_path[512];

    chunk *chunks; 
    uint32_t chunk_count;
    uint32_t chunk_cap; // number of chunk that can receive
    char cid[HASH_HEX_LEN + 1];
} upload_S;


// first 5 byte header the payload
ssize_t read_n(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; perror("read"); return -1; }
        got += r;
    }
    return (ssize_t)got;
}

int write_all(int fd, const void* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char*)buf + sent, n - sent);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); return -1; }
        sent += (size_t)w;
    }
    return 0;
}

int send_frame(int fd, uint8_t op, const void* payload, uint32_t len) {
    uint8_t header[5];
    header[0] = op;
    uint32_t be_len = htonl(len);
    memcpy(header + 1, &be_len, 4);
    if (write_all(fd, header, 5) < 0) return -1;
    if (len && write_all(fd, payload, len) < 0) return -1;
    return 0;
}

// ERROR helper
void send_error(int fd, const char *code, const char *message) {
    char buf[512];

    int n = snprintf(buf, sizeof(buf),
                     "{\"code\":\"%s\",\"message\":\"%s\"}",
                     code, message);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }

    send_frame(fd, OP_ERROR, buf, (uint32_t)n);
}

// should fix it!!!!!!!! not all chunk are the given size
void chunk_hash_hex(const uint8_t *data, size_t len, char out_hex[HASH_HEX_LEN + 1]) {
    // FNV-1a 64-bit
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }

    static const char *hex = "0123456789abcdef";

    int bytes = HASH_HEX_LEN / 2;   // 8 bytes -> 16 character
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(h >> (8 * i));
        out_hex[i * 2]     = hex[(b >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[b & 0xF];
    }
    out_hex[HASH_HEX_LEN] = '\0';
}


void ensure_dir(const char *path) {
    if (mkdir(path, 0777) < 0) {
        if (errno != EEXIST) {
            perror("mkdir");
        }
    }
}

int file_exists(const char *path){
    struct stat st;
    return stat(path, &st)==0;
}

uint8_t *read_file_into_buf(const char* path , size_t *output_size){
    FILE *rf = fopen(path, "rb");
    if (!rf) return NULL;
    if (fseek(rf, 0, SEEK_END) != 0) {
        fclose(rf);
        return NULL;
    }
    //current file offset (we just seeked the end -> give us file size)
    long s = ftell(rf);
    if (s<0){
        fclose(rf);
        return NULL;
    }
    //move file position to the start
    rewind(rf);
    uint8_t *buf = malloc((size_t)s+1);
    if (!buf){
        fclose(rf);
        return NULL;
    }
    //read s byte from file to buf
    size_t rd = fread(buf,1,(size_t)s,rf);
    fclose(rf);
    //If the number of bytes read is different from the expected file size:
    // free the allocated buffer (prevent memory leak)
    if (rd!=(size_t)s){
        free(buf);
        return NULL;
    }
    buf[rd] = '\0';
    //store the actual bytes read there (tell the length of the returned data)
    if (output_size) *output_size=rd;
    return buf;
}
int parse_manifest_chunks(const uint8_t *buf , size_t buf_length , chunk **output_chunks){
    const char *s = (const char*)buf;
    // look for substring "chunks" in manifest
    const char *chunks_pos = strstr(s, "\"chunks\"");
    if (!chunks_pos) return -1;
    const char *arr_start = strchr(chunks_pos, '[');
    if (!arr_start) return -1;
    const char *p = arr_start + 1;
    // space for 16 chunk
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
        // extract the hash
        // has_pos->'"'hash": "something"
        // q->'"'hash": "something"
        const char *q = strchr(hash_pos, '"');
        if (!q) {
            free(arr);
            return -1;
        }
        // find the closing quote of "hash"
        // "hash"q->':' "something"
        q = strchr(q + 1, '"');
        if (!q) {
            free(arr);
            return -1;
        }
        // find start of actual hash string
        // "hash": val_start->'"'something"
        const char *val_start = strchr(q + 1, '"');
        if (!val_start) {
            free(arr);
            return -1;
        }
        // "hash": "val_start->'s'omething"
        val_start++;
        // "hash": "something val_start->'"'
        const char *val_end = strchr(val_start, '"');
        if (!val_end) {
            free(arr);
            return -1;
        }
        // val_start = start of string
        // val_end = closing quote
        // compute the length = val_end - val_start
        size_t h_len = (size_t) (val_end - val_start);
        if (h_len != HASH_HEX_LEN){
            if (h_len > HASH_HEX_LEN) h_len = HASH_HEX_LEN;
        }
        // double the array size if there are more than 16 chunks
        if (count == cap) {
            uint32_t ncap = cap * 2;
            chunk *narr = realloc(arr, ncap * sizeof(chunk));
            if (!narr) { free(arr); return -1; }
            arr = narr;
            cap = ncap;
        }
        //fill the chunk struct
        arr[count].index = (uint32_t)idx_val;
        arr[count].size  = (uint32_t)size_val;
        //clear hash buffer and copy hash character to array
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

#define DOWNLOAD_BUF_SZ (256 * 1024)

int send_block_chunk(int cfd, const chunk *ci) {
    // build block path: blocks/aa/bb/<hash>.chunk
    //Declares three strings to hold directory paths and the full file path
    char dir1[64], dir2[128], filepath[512];
    // build the first level directory from the first 2 character
    snprintf(dir1, sizeof(dir1), "blocks/%c%c", ci->hash[0], ci->hash[1]);
    // build the first level directory from character 3 and 4 of the hash (blocks/aa/bb/)
    snprintf(dir2, sizeof(dir2), "%s/%c%c", dir1, ci->hash[2], ci->hash[3]);
    // build the full file path blocks/aa/bb/<hash>.chunk
    snprintf(filepath, sizeof(filepath), "%s/%s.chunk", dir2, ci->hash);
    if (!file_exists(filepath)) {
        send_error(cfd, "E_NOT_FOUND", "Block file missing");
        return -1;
    }
    // open in binary mode
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_error(cfd, "E_IO", "Cannot open block file");
        return -1;
    }
    // compute actual file size
    //move the file pointer to the end
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_error(cfd, "E_IO", "Seek error");
        return -1;
    }
    // get the size
    long fsz = ftell(f);
    if (fsz < 0) {
        fclose(f);
        send_error(cfd, "E_IO", "ftell error");
        return -1;
    }
    // back to start
    rewind(f);
    if ((uint32_t)fsz != ci->size) {
        // size mismatch
        fclose(f);
        send_error(cfd, "E_HASH_MISMATCH", "Block size mismatch");
        return -1;
    }

    // temporary buffer for reading data
    uint8_t buf[DOWNLOAD_BUF_SZ];
    // buffer to hold the entire chunk
    uint8_t *all = malloc(ci->size);
    if (!all) {
        fclose(f);
        send_error(cfd, "E_MEM", "Cannot alloc chunk buffer");
        return -1;
    }
    // read the entire file into memory
    size_t read_total = fread(all, 1, ci->size, f);
    // close it
    fclose(f);
    if (read_total != ci->size) {
        free(all);
        send_error(cfd, "E_IO", "Cannot read full block");
        return -1;
    }
    // verify hash:
    // compute the hex string of the chunk's hash
    char computed_hash[HASH_HEX_LEN + 1];
    chunk_hash_hex(all, read_total, computed_hash);
    // compare the computed with the expected
    if (strncmp(computed_hash, ci->hash, HASH_HEX_LEN) != 0) {
        free(all);
        send_error(cfd, "E_HASH_MISMATCH", "Hash mismatch for block");
        return -1;
    }
    // send OP_DOWNLOAD_CHUNK frames
    if (send_frame(cfd, OP_DOWNLOAD_CHUNK, all, (uint32_t)read_total) < 0) {
        free(all);
        return -1;
    }

    /*
    // stream the chunk in DOWNLOAD_BUF_SZ frames
    size_t left = read_total;
    size_t offset = 0;
    while (left > 0) {
        uint32_t step = (left > DOWNLOAD_BUF_SZ) ? DOWNLOAD_BUF_SZ : (uint32_t)left;
        if (send_frame(cfd, OP_DOWNLOAD_CHUNK, all + offset, step) < 0) {
            free(all);
            return -1;
        }
        offset += step;
        left -= step;
    }
     */

    // free memory
    free(all);
    return 0;
}


void handle_connection(int cfd) {
    /*
     * build a structure for the file that should be uploaded
     * it contains :
     * name
     * whole size
     * chunk size
     * hash algo
     * here we define : initialize value in "start" then use it later
    */
    upload_S up;
    memset(&up, 0, sizeof(up));
        
    for (;;) {
        uint8_t header[5];
        ssize_t r = read_n(cfd, header, 5); 
        if (r == 0) break;
        if (r < 0)  break;

        uint8_t op = header[0];
        uint32_t len;
        memcpy(&len, header + 1, 4);
        len = ntohl(len);
        uint8_t* payload = NULL;
        if (len) {
            payload = (uint8_t*)malloc(len);
            if (!payload) { perror("malloc"); break; }
            if (read_n(cfd, payload, len) <= 0) { free(payload); break; }
        }
        

        if (op == OP_UPLOAD_START) {
            // TODO: initialize upload state

            printf("[ENGINE] OP_UPLOAD_START received\n");

            upload_S *sess = &up;

            // if it chunks before then erase
            if (sess->chunks) {
                free(sess->chunks);
                sess->chunks = NULL;
                sess->chunk_count = 0;
                sess->chunk_cap = 0;
            }

            memset(sess->filename, 0, sizeof(sess->filename));
            memset(sess->hash_algo, 0, sizeof(sess->hash_algo));
            memset(sess->temp_manifest_path, 0, sizeof(sess->temp_manifest_path));
            sess->total_size = 0;

            size_t fn_len = len;
            if (fn_len >= sizeof(sess->filename))
                fn_len = sizeof(sess->filename) - 1;

            memcpy(sess->filename, payload, fn_len);
            sess->filename[fn_len] = '\0';

            sess->chunk_size = 256 * 1024;
            strncpy(sess->hash_algo, "blake3", sizeof(sess->hash_algo)-1);

            snprintf(sess->temp_manifest_path,
                     sizeof(sess->temp_manifest_path),
                     "manifests/%s.tmp", sess->filename);

            
            printf("[ENGINE] UPLOAD_START: name=\"%s\"\n", sess->filename);
            printf("#chunk_size=%u algo=%s\n", sess->chunk_size, sess->hash_algo);

            fflush(stdout);
        } 
        else if (op == OP_UPLOAD_CHUNK) {
            // TODO: process chunk (hash/store); here just drop
            upload_S *sess = &up;
            
            if (sess->filename[0] == '\0') {
                send_error(cfd, "E_PROTO", "UPLOAD_CHUNK before UPLOAD_START");
                break;
            }

            // Hash the chunks
            char hash_hex[HASH_HEX_LEN + 1];
            chunk_hash_hex(payload, len, hash_hex);

            //blocks/ab/cd/hash
            ensure_dir("blocks");

            //Building nested folder
            char dir1[64];
            snprintf(dir1, sizeof(dir1), "blocks/%c%c", hash_hex[0], hash_hex[1]);
            ensure_dir(dir1);

            char dir2[64];
            snprintf(dir2, sizeof(dir2), "blocks/%c%c/%c%c", hash_hex[0], hash_hex[1], hash_hex[2], hash_hex[3]);
            ensure_dir(dir2);

            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s.chunk", dir2, hash_hex);

            // Save chunks there
            FILE *fp = fopen(filepath, "wb");
            if (!fp) {
                perror("fopen block");
                send_error(cfd, "E_IO", "Cannot open block file");
                break;
            }

            if (len > 0 && payload) {
                size_t written = fwrite(payload, 1, len, fp);
                if (written != len) {
                    perror("fwrite block");
                    send_error(cfd, "E_IO", "Failed to write block");
                    fclose(fp);
                    break;
                }
            }
            fclose(fp);

            // We get the total file size from the number of chunks we are storing
            sess->total_size += len;
            
            // Increase it when it's full (because we don't know the file's size)
            if (sess->chunk_count == sess->chunk_cap) {
                uint32_t new_cap;
                if(sess->chunk_cap == 0) new_cap = 16;
                else new_cap =  sess->chunk_cap * 2;
                chunk *new_arr = realloc(sess->chunks, new_cap * sizeof(chunk));
                
                if (!new_arr) {
                    perror("realloc chunks");
                    send_error(cfd, "E_MEM", "Cannot grow chunk table");
                    break;
                }
                sess->chunks = new_arr;
                sess->chunk_cap = new_cap;
            }

            // Adding a new chunk
            uint32_t idx = sess->chunk_count;
            sess->chunks[idx].index = idx;
            sess->chunks[idx].size  = (uint32_t)len;
            memcpy(sess->chunks[idx].hash, hash_hex, HASH_HEX_LEN);
            sess->chunks[idx].hash[HASH_HEX_LEN] = '\0';
            sess->chunk_count++;

        } 
        else if (op == OP_UPLOAD_FINISH) {
            upload_S *sess = &up;

            printf("[ENGINE] UPLOAD_FINISH for \"%s\", total_size=%llu\n",
                sess->filename, (unsigned long long)sess->total_size);

            ensure_dir("manifests");

            // 1) Write the manifest temporarily in a filename-based file
            FILE *mf = fopen(sess->temp_manifest_path, "w");
            if (!mf) {
                perror("fopen manifest");
                send_error(cfd, "E_IO", "Cannot write manifest");
                free(payload);
                break;
            }

            fprintf(mf,
                    "{"
                    "\"version\":1,"
                    "\"hash_algo\":\"%s\","
                    "\"chunk_size\":%u,"
                    "\"total_size\":%llu,"
                    "\"filename\":\"%s\","
                    "\"chunks\":[",
                    sess->hash_algo,
                    sess->chunk_size,
                    (unsigned long long)sess->total_size,
                    sess->filename);

            for (uint32_t i = 0; i < sess->chunk_count; i++) {
                chunk *ci = &sess->chunks[i];
                fprintf(mf,
                        "%s{\"index\":%u,\"size\":%u,\"hash\":\"%s\"}",
                        (i > 0 ? "," : ""),
                        ci->index,
                        ci->size,
                        ci->hash);
            }

            fprintf(mf, "]}\n");
            fclose(mf);

            // 2) Now we read the manifest from this file to get a hash on it
            FILE *rf = fopen(sess->temp_manifest_path, "rb");
            if (!rf) {
                perror("fopen manifest for hash");
                send_error(cfd, "E_IO", "Cannot reopen manifest");
                free(payload);
                continue;
            }

            if (fseek(rf, 0, SEEK_END) != 0) {
                perror("fseek manifest");
                fclose(rf);
                send_error(cfd, "E_IO", "Seek error on manifest");
                free(payload);
                continue;
            }

            long sz = ftell(rf);
            if (sz < 0) {
                perror("ftell manifest");
                fclose(rf);
                send_error(cfd, "E_IO", "Size error on manifest");
                free(payload);
                continue;
            }
            rewind(rf);

            uint8_t *buf = (uint8_t*)malloc((size_t)sz);
            if (!buf) {
                perror("malloc manifest buffer");
                fclose(rf);
                send_error(cfd, "E_MEM", "Cannot alloc manifest buffer");
                free(payload);
                continue;
            }

            size_t rd = fread(buf, 1, (size_t)sz, rf);
            fclose(rf);
            if (rd != (size_t)sz) {
                perror("fread manifest");
                free(buf);
                send_error(cfd, "E_IO", "Cannot read manifest fully");
                free(payload);
                continue;
            }

            // 3) hash(manifest) → CID
            char cid_hex[HASH_HEX_LEN + 1];
            chunk_hash_hex(buf, (size_t)sz, cid_hex);
            free(buf);

            memcpy(sess->cid, cid_hex, HASH_HEX_LEN);
            sess->cid[HASH_HEX_LEN] = '\0';

            printf("[ENGINE] computed CID=%s\n", cid_hex);

            // 4) Move the final manifest to the CID name: manifests/<cid>.json
            char final_manifest_path[512];
            snprintf(final_manifest_path, sizeof(final_manifest_path),
                    "manifests/%s.json", cid_hex);

            if (rename(sess->temp_manifest_path, final_manifest_path) < 0) {
                perror("rename manifest");
                // If the rename fails, we still return the CID
            }

            // 5) Return the CID to the gateway (plain string)
            send_frame(cfd, OP_UPLOAD_DONE, cid_hex, (uint32_t)strlen(cid_hex));

            printf("[ENGINE] UPLOAD_FINISH -> returning CID %s\n", cid_hex);
            fflush(stdout);
        }

        else if (op == OP_DOWNLOAD_START) {
            printf("[ENGINE] DOWNLOAD_START: cid=\"%.*s\"\n", (int)len, (char*)payload);
            fflush(stdout);
            // TODO: look up CID, stream verified chunks
            // Minimal placeholder: no chunks, just DONE

            //copy cid into local buffer
            char cid_buf[HASH_HEX_LEN+1];
            // if payload is too long , make it the size of buffer
            size_t cid_len = len;
            if(cid_len >= sizeof(cid_buf)){
                cid_len = sizeof(cid_buf)-1;
            }
            //copy the cid into string
            memcpy(cid_buf,payload,cid_len);
            cid_buf[cid_len]='\0';
            //check if the cid is valid
            for (size_t i = 0; i < cid_len; i++) {
                char c = cid_buf[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    send_error(cfd, "E_BAD_CID", "Invalid CID");
                    break;
                }
            }
            //building manifest path
            char manifest_path[512];
            snprintf(manifest_path, sizeof (manifest_path), "manifests/%s.json" , cid_buf);
            //check for manifest existence
            if (!file_exists(manifest_path)){
                send_error(cfd, "E_NOT_FOUND", "Manifest not found");
            } else {
                // if it exists then read it into memory
                size_t mf_size = 0;
                uint8_t *mf_buf = read_file_into_buf(manifest_path, &mf_size);
                if (!mf_buf) {
                    send_error(cfd, "E_IO", "Cannot read manifest");
                } else {
                    chunk *chunks = NULL;
                    int n_chunks = parse_manifest_chunks(mf_buf, mf_size, &chunks);
                    if (n_chunks <= 0) {
                        free(mf_buf);
                        send_error(cfd, "E_PROTO", "Malformed manifest");
                    } else{
                        int everything_ok=1;
                        // iterate in correct order of chunks
                        for (int i = 0; i < n_chunks; i++) {
                            chunk *ci = &chunks[i];
                            if (send_block_chunk(cfd, ci) < 0) {
                                everything_ok = 0;
                                break;
                            }
                        }
                        if (everything_ok) {
                            send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
                        }
                        // cleanup memory
                        free(chunks);
                        free(mf_buf);
                    }
                }
            }
            send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
        } 
        else {
        }

        free(payload);
    }

    close(cfd);
}


// The pthread format that is written has a warning
// Writing the "sent" format as the function required
// I guess fix this ????????
void* connection_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    handle_connection(cfd);
    return NULL;
}


int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/cengine.sock\n", argv[0]);
        return 2;
    }
    g_sock_path = argv[1];

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
    unlink(g_sock_path);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 2; }
    if (listen(fd, 64) < 0) { perror("listen"); return 2; }

    printf("[ENGINE] listening on %s\n", g_sock_path);
    fflush(stdout);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pthread_t th;
        pthread_create(&th, NULL, connection_thread, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(fd);
    unlink(g_sock_path);
    return 0;
}

