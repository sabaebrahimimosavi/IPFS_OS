#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "upload.h"

int parse_manifest_chunks(const uint8_t *buf , size_t buf_length , chunk **output_chunks);
int send_block_chunk(int cfd, const chunk *ci);
void handle_download(int cfd, const uint8_t *payload, uint32_t len);

#endif
