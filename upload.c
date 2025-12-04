#include "upload.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_UPLOAD_DONE   0x81

extern int send_frame(int fd, uint8_t op, const void* payload, uint32_t len);
extern void send_error(int fd, const char *code, const char *message);

void handle_upload_start(int cfd, upload_S *sess,const uint8_t *payload,uint32_t len){

    memset(sess,0,sizeof(*sess));

    size_t fn_len=len;
    if(fn_len>=sizeof(sess->filename))
        fn_len=sizeof(sess->filename)-1;

    memcpy(sess->filename,payload,fn_len);
    sess->filename[fn_len]='\0';

    sess->chunk_size=256*1024;
    strncpy(sess->hash_algo,"blake3",sizeof(sess->hash_algo)-1);

    snprintf(sess->temp_manifest_path,sizeof(sess->temp_manifest_path),
             "manifests/%s.tmp",sess->filename);

    printf("[ENGINE] UPLOAD_START: %s\n",sess->filename);
}

void handle_upload_chunk(int cfd,upload_S *sess,const uint8_t *payload,uint32_t len){

    if(sess->filename[0]=='\0'){
        send_error(cfd,"E_PROTO","UPLOAD_CHUNK before UPLOAD_START");
        return;
    }

    char hash_hex[HASH_HEX_LEN+1];
    chunk_hash_hex(payload,len,hash_hex);

    ensure_dir("blocks");

    char dir1[64],dir2[64],filepath[256];
    snprintf(dir1,sizeof(dir1),"blocks/%c%c",hash_hex[0],hash_hex[1]);
    ensure_dir(dir1);

    snprintf(dir2,sizeof(dir2),"%s/%c%c",dir1,hash_hex[2],hash_hex[3]);
    ensure_dir(dir2);

    snprintf(filepath,sizeof(filepath),"%s/%s.chunk",dir2,hash_hex);

    FILE *fp=fopen(filepath,"wb");
    if(!fp){ send_error(cfd,"E_IO","cannot write chunk"); return; }
    fwrite(payload,1,len,fp);
    fclose(fp);

    sess->total_size+=len;

    if(sess->chunk_count==sess->chunk_cap){
        sess->chunk_cap = sess->chunk_cap? sess->chunk_cap*2:16;
        sess->chunks = realloc(sess->chunks,sizeof(chunk)*sess->chunk_cap);
    }

    chunk *ci = &sess->chunks[sess->chunk_count++];
    ci->index = sess->chunk_count-1;
    ci->size  = len;
    memcpy(ci->hash,hash_hex,HASH_HEX_LEN+1);
}

void handle_upload_finish(int cfd,upload_S *sess){

    ensure_dir("manifests");

    FILE *mf=fopen(sess->temp_manifest_path,"w");
    fprintf(mf,
            "{ \"version\":1,\"hash_algo\":\"%s\",\"chunk_size\":%u,"
            "\"total_size\":%llu,\"filename\":\"%s\",\"chunks\":[",
            sess->hash_algo,sess->chunk_size,
            (unsigned long long)sess->total_size,sess->filename);

    for(uint32_t i=0;i<sess->chunk_count;i++){
        chunk *ci=&sess->chunks[i];
        fprintf(mf,"%s{\"index\":%u,\"size\":%u,\"hash\":\"%s\"}",
                (i?",":""),ci->index,ci->size,ci->hash);
    }
    fprintf(mf,"]}");
    fclose(mf);

    size_t mf_size;
    uint8_t *buf=read_file_into_buf(sess->temp_manifest_path,&mf_size);

    char cid_hex[HASH_HEX_LEN+1];
    chunk_hash_hex(buf,mf_size,cid_hex);
    free(buf);

    memcpy(sess->cid,cid_hex,HASH_HEX_LEN+1);

    char final_path[512];
    snprintf(final_path,sizeof(final_path),"manifests/%s.json",cid_hex);
    rename(sess->temp_manifest_path,final_path);

    send_frame(cfd,OP_UPLOAD_DONE,cid_hex,strlen(cid_hex));

    printf("[ENGINE] UPLOAD_FINISH -> CID=%s\n",cid_hex);

    /* RESET SESSION — MATCH OLD ENGINE */
    if(sess->chunks) free(sess->chunks);
    memset(sess,0,sizeof(*sess));
}
