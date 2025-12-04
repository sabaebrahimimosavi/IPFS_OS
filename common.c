#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

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

void chunk_hash_hex(const uint8_t *data, size_t len, char out_hex[HASH_HEX_LEN + 1]) {
    uint64_t h = 1469598103934665603ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }

    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < HASH_HEX_LEN/2; i++) {
        uint8_t b = (uint8_t)(h >> (8 * i));
        out_hex[i * 2]     = hex[(b >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[b & 0xF];
    }
    out_hex[HASH_HEX_LEN] = '\0';
}
