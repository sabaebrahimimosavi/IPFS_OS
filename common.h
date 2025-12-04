#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>

// BLAKE3 = 32 bytes = 64 hex chars
#define HASH_HEX_LEN 64

void ensure_dir(const char *path);
int file_exists(const char *path);
uint8_t *read_file_into_buf(const char* path , size_t *output_size);
void chunk_hash_hex(const uint8_t *data, size_t len, char out_hex[HASH_HEX_LEN + 1]);

#endif
