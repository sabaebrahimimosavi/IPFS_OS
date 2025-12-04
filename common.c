#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "blake3.h"

void ensure_dir(const char *path) {
    if (mkdir(path, 0777) < 0 && errno != EEXIST) {
        perror("mkdir");
    }
}

int file_exists(const char *path){
    struct stat st;
    return stat(path, &st)==0;
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

