// Build: gcc -O2 -pthread -o c_engine c_engine.c upload.c download.c common.c
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

#include "upload.h"
#include "download.h"

#define OP_UPLOAD_START  0x01
#define OP_UPLOAD_CHUNK  0x02
#define OP_UPLOAD_FINISH 0x03
#define OP_UPLOAD_DONE   0x81

#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

#define OP_ERROR 0xFF
static const char* g_sock_path = NULL;

thread_pool_t g_pool;
#define NUM_WORKER_THREADS 8 

//low-level I/O helpers (kept in main file as in your base)
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

// send a framed message: 1 byte opcode + 4 byte big-endian length + payload
int send_frame(int fd, uint8_t op, const void* payload, uint32_t len) {
    uint8_t header[5];
    header[0] = op;
    uint32_t be_len = htonl(len);
    memcpy(header + 1, &be_len, 4);
    if (write_all(fd, header, 5) < 0) return -1;
    if (len && write_all(fd, payload, len) < 0) return -1;
    return 0;
}

// small JSON error helper used by upload/download modules
void send_error(int fd, const char *code, const char *message) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"code\":\"%s\",\"message\":\"%s\"}",
                     code, message ? message : "");
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    send_frame(fd, OP_ERROR, buf, (uint32_t)n);
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

    upload_S sess;
    memset(&sess, 0, sizeof(sess));
    sess.chunk_cap = 0;
    sess.chunk_count = 0;
    sess.chunks = NULL;

    for (;;) {
        uint8_t header[5];
        ssize_t r = read_n(cfd, header, 5);
        if (r == 0) break;
        if (r < 0) { break; }

        uint8_t op = header[0];
        uint32_t len;
        memcpy(&len, header + 1, 4);
        len = ntohl(len);

        uint8_t* payload = NULL;
        if (len) {
            payload = (uint8_t*)malloc((size_t)len);
            if (!payload) { perror("malloc"); break; }
            if (read_n(cfd, payload, (size_t)len) <= 0) { free(payload); break; }
        }

        if(op==OP_UPLOAD_START)   handle_upload_start(cfd,&sess,payload,len);
        else if(op==OP_UPLOAD_CHUNK) handle_upload_chunk(cfd,&sess,payload,len);
        else if(op==OP_UPLOAD_FINISH) handle_upload_finish(cfd,&sess);
        else if (op == OP_DOWNLOAD_START) {
            printf("[ENGINE] DOWNLOAD_START: cid=\"%.*s\"\n", (int)len, (char*)payload ? (char*)payload : "");
            fflush(stdout);
            // delegate download handling (stream chunks and DONE)
            handle_download(cfd, payload, len);
        }
        else {
            // unknown op: ignore or send error
            send_error(cfd, "E_PROTO", "Unknown opcode");
        }

        free(payload);
    }

    // cleanup on connection close
    if (sess.chunks) free(sess.chunks);
    close(cfd);
}

// wrapper for pthreads: safe cast
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

    thread_pool_init(&g_pool, NUM_WORKER_THREADS);

    printf("[ENGINE] listening on %s\n", g_sock_path);
    fflush(stdout);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        // Thread-per-connection keeps it readable for OS labs
        pthread_t th;
        if (pthread_create(&th, NULL, connection_thread, (void*)(intptr_t)cfd) != 0) {
            perror("pthread_create");
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }

    thread_pool_destroy(&g_pool);

    close(fd);
    unlink(g_sock_path);
    return 0;
}



