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
#include <sys/stat.h>   // برای mkdir

#define OP_UPLOAD_START  0x01
#define OP_UPLOAD_CHUNK  0x02
#define OP_UPLOAD_FINISH 0x03
#define OP_UPLOAD_DONE   0x81

#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

#define OP_ERROR 0xFF
#define HASH_HEX_LEN 16   // خروجی تابع hash 32 بایت- میشه 64 کاراکتر


static const char* g_sock_path = NULL;

typedef struct{
        uint32_t index;
        uint32_t size;
        char hash[HASH_HEX_LEN + 1];  // هش به صورت hex
}chunk;


typedef struct {
    char filename[256];
    uint64_t total_size;
    uint32_t chunk_size;
    char hash_algo[32];
    char temp_manifest_path[512];

    chunk *chunks; 
    uint32_t chunk_count;
    uint32_t chunk_cap; // تعداد کل چانک هایی که میتونه دریافت کنه
    char cid[HASH_HEX_LEN + 1];
} upload_S;


// اول هدر 5 بایتی رو میگیره بعد payload
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

// ---- ERROR helper ----
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

// این باید درست بشه که همه جانک ها لزوما اون سایز گفته شده نیستن!
void chunk_hash_hex(const uint8_t *data, size_t len, char out_hex[HASH_HEX_LEN + 1]) {
    // FNV-1a 64-bit
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }

    static const char *hex = "0123456789abcdef";

    int bytes = HASH_HEX_LEN / 2;   // مثلاً 16 کاراکتر → 8 بایت
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

void handle_connection(int cfd) {
    // یه ساختار میسازیم برای فایلی که قراره اپلود بشه که اسم و سایز کل و سایز چانک
    // و الگو هش داخلشه. اینجا تعریف میکنیم که توی start مقدار بدیم بعدا هم ازش استفاده کنیم
    
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

            // اگه قبلا چانک شده پاک میکنیم
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

            // چانک ها رو هش میکنیم
            char hash_hex[HASH_HEX_LEN + 1];
            chunk_hash_hex(payload, len, hash_hex);

            //blocks/ab/cd/hash
            ensure_dir("blocks");

            //داریم پوشه تو در تو میسازیم
            char dir1[64];
            snprintf(dir1, sizeof(dir1), "blocks/%c%c", hash_hex[0], hash_hex[1]);
            ensure_dir(dir1);

            char dir2[64];
            snprintf(dir2, sizeof(dir2), "blocks/%c%c/%c%c", hash_hex[0], hash_hex[1], hash_hex[2], hash_hex[3]);
            ensure_dir(dir2);

            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s.chunk", dir2, hash_hex);

            // حالا چانک ها رو باید اونجا ذخیره کنیم
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

            // از روی تعداد چانک هایی که داریم ذخیره میکنیم باید اندازه کل فایل رو به دست بیاریم
            sess->total_size += len;
            
            // چون نمیدونیم اندازه فایل چقدره وقتی پر شد میایم افزایش میدیم   
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

            // چانک جدید اصافع میکنه
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

            // 1) manifest را به صورت موقت بنویسیم در فایل filename-based
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

            // 2) حالا manifest را از روی همین فایل می‌خوانیم تا روی آن hash بگیریم
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

            // 4) manifest نهایی را به اسم CID جابه‌جا کن: manifests/<cid>.json
            char final_manifest_path[512];
            snprintf(final_manifest_path, sizeof(final_manifest_path),
                    "manifests/%s.json", cid_hex);

            if (rename(sess->temp_manifest_path, final_manifest_path) < 0) {
                perror("rename manifest");
                // اگه rename fail شد، ولی باز هم CID رو برمی‌گردونیم.
            }

            // 5) CID را به gateway برگردانیم (رشته ساده)
            send_frame(cfd, OP_UPLOAD_DONE, cid_hex, (uint32_t)strlen(cid_hex));

            printf("[ENGINE] UPLOAD_FINISH -> returning CID %s\n", cid_hex);
            fflush(stdout);
        }

        else if (op == OP_DOWNLOAD_START) {
            printf("[ENGINE] DOWNLOAD_START: cid=\"%.*s\"\n", (int)len, (char*)payload);
            fflush(stdout);
            // TODO: look up CID, stream verified chunks
            // Minimal placeholder: no chunks, just DONE
            send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
        } 
        else {
        }

        free(payload);
    }

    close(cfd);
}

// فرمت pthread که اول نوشتن وارنینگ میده برای همین فرمت ارسالی رو اونطوری که تابع می خواد داره مینویسه
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

