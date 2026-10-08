#include "ndef.h"
#include <string.h>
int ndef_encode_title(const char *title,uint8_t out[NDEF_TITLE_SIZE]) {
    if(!valid_title(title)) return -1;
    memset(out,0,NDEF_TITLE_SIZE);
    out[0]=3; out[1]=23; out[2]=0xd1; out[3]=1; out[4]=19; out[5]='T'; out[6]=2;
    memcpy(out+7,"en",2); memcpy(out+9,"ps5nfc:",7); memcpy(out+16,title,9); out[25]=0xfe;
    return 0;
}
static int record_title(const uint8_t *payload,size_t length,char title[TITLE_CAP]) {
    if(length>=7 && !memcmp(payload,"ps5nfc:",7)) { payload+=7; length-=7; }
    if(length!=9) return 0;
    memcpy(title,payload,9); title[9]=0;
    if(valid_title(title)) return 1;
    title[0]=0; return 0;
}
static int decode_records(const uint8_t *data,size_t length,char title[TITLE_CAP]) {
    size_t pos=0; int found=0,first=1,ended=0;
    while(pos<length) {
        if(ended || length-pos<3) return -1;
        uint8_t flags=data[pos++],type_length=data[pos++];
        if((flags&0x20) || (first?!(flags&0x80):(flags&0x80))) return -1;
        size_t payload_length;
        if(flags&0x10) payload_length=data[pos++];
        else {
            if(length-pos<4) return -1;
            payload_length=(size_t)data[pos]<<24|(size_t)data[pos+1]<<16|(size_t)data[pos+2]<<8|data[pos+3]; pos+=4;
        }
        size_t id_length=0;
        if(flags&8) { if(pos==length) return -1; id_length=data[pos++]; }
        if(type_length>length-pos) return -1;
        const uint8_t *type=data+pos; pos+=type_length;
        if(id_length>length-pos) return -1;
        pos+=id_length;
        if(payload_length>length-pos) return -1;
        const uint8_t *payload=data+pos; pos+=payload_length;
        char candidate[TITLE_CAP]={0}; int valid=0;
        if((flags&7)==1 && type_length==1 && payload_length) {
            if(type[0]=='U' && payload[0]==0) valid=record_title(payload+1,payload_length-1,candidate);
            if(type[0]=='T' && !(payload[0]&0x80)) {
                size_t language=payload[0]&0x3f;
                if(language>payload_length-1) return -1;
                valid=record_title(payload+1+language,payload_length-1-language,candidate);
            }
        }
        if(valid) {
            if(found && strcmp(title,candidate)) return -1;
            strcpy(title,candidate); found=1;
        }
        first=0; ended=(flags&0x40)!=0;
    }
    return ended?found:-1;
}
int ndef_decode_title(const uint8_t *data,size_t len,char title[TITLE_CAP]) {
    title[0]=0;
    size_t i=0;
    while(i<len) {
        uint8_t type=data[i++];
        if(type==0) continue;
        if(type==0xfe || i>=len) return 0;
        size_t n=data[i++];
        if(n==255) {
            if(i+2>len) return -1;
            n=(size_t)data[i]<<8|data[i+1]; i+=2;
        }
        if(n>len-i) return -1;
        if(type==3) {
            if(!n) return 0;
            int result=decode_records(data+i,n,title);
            if(result!=1) title[0]=0;
            return result;
        }
        i+=n;
    }
    return 0;
}
