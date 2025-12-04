#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "download.h"
#include "common.h"

extern void send_error(int fd, const char *code, const char *message);
extern int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);

#define DOWNLOAD_BUF_SZ (256 * 1024)
#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

/* -----------------------------------------------------------
   1) Parse manifest JSON into chunk[]
   (EXACT logic taken from your original code)
   ----------------------------------------------------------- */
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



/* -----------------------------------------------------------
   2) Send a single chunk file (with hash verification)
   (EXACT logic taken from your original code)
   ----------------------------------------------------------- */
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



/* -----------------------------------------------------------
   3) Main DOWNLOAD logic
   (EXACT behavior from your code)
   ----------------------------------------------------------- */
void handle_download(int cfd, const uint8_t *payload, uint32_t len) {
    //copy cid into local buffer
    char cid_buf[HASH_HEX_LEN + 1];
    // if payload is too long , make it the size of buffer
    size_t cid_len = len;
    if (cid_len >= sizeof(cid_buf))
        cid_len = sizeof(cid_buf) - 1;
    //copy the cid into string
    memcpy(cid_buf, payload, cid_len);
    cid_buf[cid_len] = '\0';
    //check if the cid is valid
    for (size_t i = 0; i < cid_len; i++) {
        char c = cid_buf[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            send_error(cfd, "E_BAD_CID", "Invalid CID");
            return;
        }
    }
    //building manifest path
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path),"manifests/%s.json", cid_buf);
    //check for manifest existence
    if (!file_exists(manifest_path)) {
        send_error(cfd, "E_NOT_FOUND", "Manifest not found");
        return;
    }
    // if it exists then read it into memory
    size_t mf_size = 0;
    uint8_t *mf_buf = read_file_into_buf(manifest_path, &mf_size);
    if (!mf_buf) {
        send_error(cfd, "E_IO", "Cannot read manifest");
        return;
    }

    chunk *chunks = NULL;
    int n_chunks = parse_manifest_chunks(mf_buf, mf_size, &chunks);
    free(mf_buf);
    if (n_chunks <= 0) {
        send_error(cfd, "E_PROTO", "Malformed manifest");
        return;
    }

    int ok = 1;
    // iterate in correct order of chunks
    for (int i = 0; i < n_chunks; i++) {
        if (send_block_chunk(cfd, &chunks[i]) < 0) {
            ok = 0;
            break;
        }
    }

    free(chunks);

    if (ok)
        send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
}
