#include "core.h"
#include <ctype.h>
#include <string.h>
#include <time.h>
uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}
int valid_uid(const char *s) {
    size_t n = strlen(s);
    if (n != 8 && n != 14 && n != 20) return 0;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
int valid_title(const char *s) {
    if (strlen(s) != 9 || (strncmp(s,"PPSA",4) && strncmp(s,"CUSA",4))) return 0;
    for (int i=4; i<9; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}
void session_forget(Session *s) {
    s->owned_app=0; s->owned_title[0]=0; s->session_hold=0; s->owned_manual=0;
}
void session_disconnect(Session *s) {
    s->reader_ready=0; s->present[0]=0; s->blocked_uid[0]=0; s->absent_since=0;
    if(!s->owned_manual) session_forget(s);
}
void session_scan(Session *s, const Config *c, const char *uid, uint64_t now, LaunchFn launch, CloseFn close_app) {
    s->reader_ready=1;
    if (!uid[0]) {
        if (!s->present[0]) return;
        if (!s->absent_since) s->absent_since=now ? now : 1;
        if (now - s->absent_since < c->removal_ms) return;
        if (s->session_hold && s->owned_app) {
            if(close_app(s->owned_app)==0) session_forget(s);
            else s->session_hold=0;
        }
        s->present[0]=0; s->blocked_uid[0]=0; s->absent_since=0;
        return;
    }
    s->absent_since=0;
    if (!strcmp(s->present, uid) || !strcmp(s->blocked_uid,uid)) return;
    if (s->session_hold && s->owned_app) {
        if (close_app(s->owned_app)) { strcpy(s->blocked_uid,uid); return; }
    }
    session_forget(s);
    strcpy(s->present,uid); strcpy(s->last_uid,uid); s->blocked_uid[0]=0;
    for (int i=0; i<c->count; i++) {
        if (strcmp(c->maps[i].uid,uid)) continue;
        unsigned app=0;
        if (launch(c->maps[i].title,&app)==0 && app) {
            s->owned_app=app; s->session_hold=c->hold;
            strcpy(s->owned_title,c->maps[i].title);
        }
        break;
    }
}
int pn532_frame(uint8_t cmd, const uint8_t *data, size_t len, uint8_t *out, size_t cap) {
    if (len>250 || cap<len+9) return -1;
    out[0]=0; out[1]=0; out[2]=0xff; out[3]=(uint8_t)(len+2); out[4]=(uint8_t)-out[3];
    out[5]=0xd4; out[6]=cmd;
    uint8_t sum=(uint8_t)(0xd4+cmd);
    for (size_t i=0;i<len;i++) { out[7+i]=data[i]; sum+=data[i]; }
    out[7+len]=(uint8_t)-sum; out[8+len]=0;
    return (int)len+9;
}
int pn532_parse(const uint8_t *b, size_t n, uint8_t cmd, uint8_t *out, size_t cap, size_t *used) {
    *used=0;
    if(n<3) return 0;
    size_t p=0;
    while(p+2<n && !(b[p]==0 && b[p+1]==0 && b[p+2]==0xff)) p++;
    if(p+2>=n) { *used=n-2; return 0; }
    *used=p;
    if(n-p<6) return 0;
    if(b[p+3]==0 && b[p+4]==0xff && b[p+5]==0) { *used=p+6; return 2; }
    if(b[p+3]==0xff && b[p+4]==0 && b[p+5]==0) { *used=p+6; return -1; }
    uint8_t len=b[p+3];
    if((uint8_t)(len+b[p+4])!=0 || len<2 || len==0xff) return -1;
    if(n-p<(size_t)len+7) return 0;
    uint8_t sum=0; for(size_t i=0;i<(size_t)len+1;i++) sum+=b[p+5+i];
    if(sum || b[p+6+len]!=0 || b[p+5]!=0xd5 || b[p+6]!=(uint8_t)(cmd+1) || (size_t)len-2>cap) return -1;
    memcpy(out,b+p+7,len-2); *used=p+len+7;
    return (int)len-2+3;
}
