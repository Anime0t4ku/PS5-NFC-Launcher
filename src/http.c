#include "app.h"
#include "games.h"
#include "cJSON.h"
#include "webui.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef FILE_ROOT
#define FILE_ROOT ""
#endif
#define HTTP_PORT 8087
static int listener=-1;
static pthread_mutex_t worker_lock=PTHREAD_MUTEX_INITIALIZER,post_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned workers;
static int send_all(int fd,const void *data,size_t len) {
    const char *p=data;
    while(len) { ssize_t n=send(fd,p,len,MSG_NOSIGNAL); if(n<=0) return -1; p+=n; len-=(size_t)n; }
    return 0;
}
static void reply(int fd,int code,const char *type,const void *data,size_t len) {
    char head[512];
    int n=snprintf(head,sizeof(head),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n\r\n",code,code==200?"OK":code==202?"Accepted":"Error",type,len);
    if(send_all(fd,head,(size_t)n)==0) send_all(fd,data,len);
}
static void json_reply(int fd,int code,cJSON *obj) {
    char *s=cJSON_PrintUnformatted(obj);
    if(s) { reply(fd,code,"application/json; charset=utf-8",s,strlen(s)); cJSON_free(s); }
    cJSON_Delete(obj);
}
static void result(int fd,int code,const char *message) {
    cJSON *o=cJSON_CreateObject(); cJSON_AddBoolToObject(o,"ok",code<300); cJSON_AddStringToObject(o,"message",message);
    json_reply(fd,code,o);
}
static const char *string_field(cJSON *o,const char *key) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(v)?v->valuestring:NULL;
}
static int header(const char *request,const char *name,char *out,size_t cap) {
    const char *p=strstr(request,"\r\n"); size_t k=strlen(name); if(!p) return 0; p+=2;
    while(*p && strncmp(p,"\r\n",2)) {
        const char *end=strstr(p,"\r\n"); if(!end) return 0;
        if((size_t)(end-p)>k && !strncasecmp(p,name,k) && p[k]==':') {
            const char *start=p+k+1; while(start<end && (*start==' ' || *start=='\t')) start++;
            if((size_t)(end-start)>=cap) return -1;
            memcpy(out,start,(size_t)(end-start)); out[end-start]=0; return 1;
        }
        p=end+2;
    }
    return 0;
}
static void log_response(int fd) {
    FILE *file=fopen(FILE_ROOT "/data/ps5-nfc-launcher/diagnostic.log","rb");
    if(!file) { result(fd,404,"Diagnostic log unavailable"); return; }
    char *data=malloc(65536);
    if(!data) { fclose(file); result(fd,503,"Log temporarily unavailable"); return; }
    if(fseek(file,0,SEEK_END)==0) {
        long size=ftell(file);
        if(size>=0) fseek(file,size>65536?size-65536:0,SEEK_SET);
    }
    size_t len=fread(data,1,65536,file); fclose(file);
    reply(fd,200,"text/plain; charset=utf-8",data,len); free(data);
}
static void state_response(int fd) {
    cJSON *o=cJSON_CreateObject(),*maps=cJSON_CreateArray();
    pthread_mutex_lock(&state_lock);
    cJSON_AddStringToObject(o,"version",VERSION);
    cJSON_AddStringToObject(o,"mode",config.hold?"hold":"tap");
    cJSON_AddNumberToObject(o,"removal_ms",config.removal_ms);
    cJSON_AddStringToObject(o,"reader",reader_status);
    cJSON_AddStringToObject(o,"usb",usb_inventory);
    cJSON_AddBoolToObject(o,"reader_ready",session.reader_ready);
    cJSON_AddStringToObject(o,"uid",session.present);
    cJSON_AddStringToObject(o,"last_uid",session.last_uid);
    cJSON_AddStringToObject(o,"card_title",card_title);
    cJSON_AddStringToObject(o,"owned_title",session.owned_title);
    cJSON_AddNumberToObject(o,"owned_app",session.owned_app);
    cJSON_AddStringToObject(o,"action",action_status);
    cJSON_AddStringToObject(o,"write",write_status);
    cJSON_AddBoolToObject(o,"launch_available",launch_available);
    cJSON_AddBoolToObject(o,"close_available",close_available);
    for(int i=0;i<config.count;i++) {
        cJSON *m=cJSON_CreateObject();
        cJSON_AddStringToObject(m,"uid",config.maps[i].uid);
        cJSON_AddStringToObject(m,"title",config.maps[i].title);
        cJSON_AddStringToObject(m,"name",config.maps[i].name);
        cJSON_AddItemToArray(maps,m);
    }
    pthread_mutex_unlock(&state_lock);
    cJSON_AddItemToObject(o,"mappings",maps); json_reply(fd,200,o);
}
static void games_response(int fd) {
    Game *games=calloc(MAX_GAMES,sizeof(*games)); if(!games) { result(fd,500,"Out of memory"); return; }
    char source[128]; int count=games_list(games,MAX_GAMES,source,sizeof(source));
    cJSON *o=cJSON_CreateObject(),*array=cJSON_CreateArray();
    cJSON_AddStringToObject(o,"source",source);
    for(int i=0;i<count;i++) {
        cJSON *g=cJSON_CreateObject();
        cJSON_AddStringToObject(g,"title",games[i].title); cJSON_AddStringToObject(g,"name",games[i].name);
        char path[64]; snprintf(path,sizeof(path),"/icon/%s.png",games[i].title);
        cJSON_AddStringToObject(g,"icon",games[i].icon?path:""); cJSON_AddItemToArray(array,g);
    }
    free(games); cJSON_AddItemToObject(o,"games",array); json_reply(fd,200,o);
}
static void icon_response(int fd,const char *url) {
    char title[TITLE_CAP],path[256];
    if(strlen(url)!=19 || strncmp(url,"/icon/",6) || strcmp(url+15,".png")) { result(fd,404,"Icon not found"); return; }
    memcpy(title,url+6,9); title[9]=0;
    if(game_icon(title,path,sizeof(path))!=0) { result(fd,404,"Icon not found"); return; }
    FILE *f=fopen(path,"rb"); if(!f) { result(fd,404,"Icon not found"); return; }
    if(fseek(f,0,SEEK_END)!=0) { fclose(f); result(fd,500,"Icon read failed"); return; }
    long size=ftell(f); rewind(f);
    if(size<=0 || size>8*1024*1024) { fclose(f); result(fd,404,"Icon unavailable"); return; }
    uint8_t *data=malloc((size_t)size);
    if(!data || fread(data,1,(size_t)size,f)!=(size_t)size) { free(data); fclose(f); result(fd,500,"Icon read failed"); return; }
    fclose(f); reply(fd,200,"image/png",data,(size_t)size); free(data);
}
static void post_response(int fd,const char *path,cJSON *o) {
    if(!strcmp(path,"/api/shutdown")) {
        pthread_mutex_lock(&state_lock); int busy=operation_busy; pthread_mutex_unlock(&state_lock);
        if(busy) { result(fd,409,"Wait for the current game action or card write to finish"); return; }
        result(fd,200,"Payload stopped");
        const char message[]="[NFC] Payload stopped from web interface\n";
        ssize_t ignored=write(STDERR_FILENO,message,sizeof(message)-1); (void)ignored;
        _exit(0);
    }
    if(!strcmp(path,"/api/launch")) {
        const char *title=string_field(o,"title");
        if(!title || !valid_title(title)) { result(fd,400,"Invalid game title ID"); return; }
        char request[TITLE_CAP];
        if(get_write_request(request)) { result(fd,409,"Finish or cancel NFC writing first"); return; }
        if(enqueue_action(title,0)) { result(fd,409,"Another action is queued"); return; }
        result(fd,202,"Launch queued"); return;
    }
    if(!strcmp(path,"/api/close")) {
        if(enqueue_action(NULL,1)) { result(fd,409,"Another action is queued"); return; }
        result(fd,202,"Close queued"); return;
    }
    if(!strcmp(path,"/api/write")) {
        const char *title=string_field(o,"title");
        if(!title || !valid_title(title)) { result(fd,400,"Invalid game title ID"); return; }
        if(arm_write(title)) { result(fd,409,"Another game or write action is active. Close the Hold-mode game before writing."); return; }
        result(fd,202,"Writing armed for two minutes. Place the card on the reader."); return;
    }
    if(!strcmp(path,"/api/write/cancel")) { if(arm_write(NULL)) result(fd,409,"A card write is in progress. Keep the card in place until verification finishes."); else result(fd,200,"Writing cancelled"); return; }
    Config copy;
    pthread_mutex_lock(&state_lock); copy=config; pthread_mutex_unlock(&state_lock);
    if(!strcmp(path,"/api/settings")) {
        const char *mode=string_field(o,"mode"); cJSON *delay=cJSON_GetObjectItemCaseSensitive(o,"removal_ms");
        if(!mode || (strcmp(mode,"tap") && strcmp(mode,"hold")) || !cJSON_IsNumber(delay) || delay->valuedouble<250 || delay->valuedouble>5000 || delay->valuedouble!=delay->valueint) {
            result(fd,400,"Choose Tap or Hold and a removal delay from 250 to 5000 ms"); return;
        }
        copy.hold=!strcmp(mode,"hold"); copy.removal_ms=(unsigned)delay->valueint;
    } else if(!strcmp(path,"/api/mapping") || !strcmp(path,"/api/mapping/delete")) {
        const char *uid=string_field(o,"uid"),*title=string_field(o,"title"),*name=string_field(o,"name");
        if(!uid || !valid_uid(uid)) { result(fd,400,"Invalid NFC UID"); return; }
        char normalized[UID_CAP]; strcpy(normalized,uid);
        for(char *p=normalized;*p;p++) *p=(char)toupper((unsigned char)*p);
        int index=-1; for(int i=0;i<copy.count;i++) if(!strcmp(copy.maps[i].uid,normalized)) index=i;
        if(!strcmp(path,"/api/mapping/delete")) {
            if(index>=0) { memmove(copy.maps+index,copy.maps+index+1,(size_t)(copy.count-index-1)*sizeof(Mapping)); copy.count--; }
        } else {
            if(!title || !valid_title(title) || !name || strlen(name)>=NAME_CAP || strpbrk(name,"\t\r\n")) {
                result(fd,400,"Invalid title ID or game name"); return;
            }
            if(index<0) { if(copy.count==MAX_MAPS) { result(fd,400,"Tag assignment limit reached"); return; } index=copy.count++; }
            Mapping *m=&copy.maps[index]; strcpy(m->uid,normalized); strcpy(m->title,title); strcpy(m->name,name);
        }
    } else { result(fd,404,"Unknown action"); return; }
    if(config_save(&copy)) { result(fd,500,"Could not save configuration"); return; }
    pthread_mutex_lock(&state_lock); config=copy; pthread_mutex_unlock(&state_lock);
    result(fd,200,"Saved. Remove and scan the card to apply its new assignment or mode.");
}
static void serve(int fd) {
    char request[8193]; size_t used=0; char *end=NULL;
    while(used<sizeof(request)-1) {
        ssize_t n=recv(fd,request+used,sizeof(request)-1-used,0); if(n<=0) return;
        used+=(size_t)n; request[used]=0; end=strstr(request,"\r\n\r\n"); if(end) break;
    }
    if(!end || (size_t)(end-request)>4096) { result(fd,400,"Invalid request headers"); return; }
    size_t header_size=(size_t)(end-request)+4;
    char method[9],path[256],protocol[16];
    if(sscanf(request,"%8s %255s %15s",method,path,protocol)!=3) { result(fd,400,"Invalid request"); return; }
    if(!strcmp(method,"GET")) {
        if(!strcmp(path,"/") || !strcmp(path,"/index.html")) reply(fd,200,"text/html; charset=utf-8",webui,webui_len);
        else if(!strcmp(path,"/api/log")) log_response(fd);
        else if(!strcmp(path,"/api/state")) state_response(fd);
        else if(!strcmp(path,"/api/games")) games_response(fd);
        else if(!strncmp(path,"/icon/",6)) icon_response(fd,path);
        else result(fd,404,"Not found");
        return;
    }
    if(strcmp(method,"POST")) { result(fd,405,"Method not allowed"); return; }
    char host[192],origin[256],expected[256],length[32],type[64],transfer[32];
    if(header(request,"Host",host,sizeof(host))!=1 || header(request,"Origin",origin,sizeof(origin))!=1) {
        result(fd,403,"Open this page directly from the PS5 address"); return;
    }
    snprintf(expected,sizeof(expected),"http://%s",host);
    if(strcmp(origin,expected)) { result(fd,403,"Origin rejected"); return; }
    if(header(request,"Transfer-Encoding",transfer,sizeof(transfer))!=0 || header(request,"Content-Length",length,sizeof(length))!=1 || header(request,"Content-Type",type,sizeof(type))!=1 || strncmp(type,"application/json",16)) {
        result(fd,400,"JSON body with Content-Length required"); return;
    }
    char *tail; unsigned long size=strtoul(length,&tail,10);
    if(!length[0] || *tail || size==0 || size>4096 || header_size+size>=sizeof(request)) { result(fd,400,"Invalid body size"); return; }
    while(used<header_size+size) {
        ssize_t n=recv(fd,request+used,header_size+size-used,0); if(n<=0) return; used+=(size_t)n;
    }
    request[header_size+size]=0;
    cJSON *o=cJSON_ParseWithLength(request+header_size,size+1);
    if(!cJSON_IsObject(o)) { cJSON_Delete(o); result(fd,400,"Invalid JSON"); return; }
    pthread_mutex_lock(&post_lock);
    post_response(fd,path,o); cJSON_Delete(o);
    pthread_mutex_unlock(&post_lock);
}
int http_prepare(void) {
    listener=socket(AF_INET,SOCK_STREAM,0); if(listener<0) return -1;
    int yes=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET; addr.sin_port=htons(HTTP_PORT); addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(listener,(struct sockaddr *)&addr,sizeof(addr))!=0 || listen(listener,16)!=0) {
        fprintf(stderr,"[NFC] Port %d unavailable: errno=%d; another instance may already be running\n",HTTP_PORT,errno);
        close(listener); listener=-1; return -1;
    }
    return 0;
}
static void *http_worker(void *arg) {
    int fd=(int)(intptr_t)arg;
    struct timeval timeout={3,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    serve(fd); close(fd);
    pthread_mutex_lock(&worker_lock); workers--; pthread_mutex_unlock(&worker_lock);
    return NULL;
}
void *http_main(void *arg) {
    (void)arg;
    pthread_attr_t attr;
    if(pthread_attr_init(&attr)!=0) return NULL;
    if(pthread_attr_setstacksize(&attr,1024*1024)!=0 || pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED)!=0) {
        pthread_attr_destroy(&attr); return NULL;
    }
    for(;;) {
        int fd=accept(listener,NULL,NULL); if(fd<0) { usleep(100000); continue; }
        pthread_mutex_lock(&worker_lock);
        int full=workers>=16; if(!full) workers++;
        pthread_mutex_unlock(&worker_lock);
        if(full) { close(fd); continue; }
        pthread_t thread;
        int rc=pthread_create(&thread,&attr,http_worker,(void *)(intptr_t)fd);
        if(rc) {
            fprintf(stderr,"[NFC] HTTP worker creation failed: %d\n",rc);
            close(fd); pthread_mutex_lock(&worker_lock); workers--; pthread_mutex_unlock(&worker_lock);
        }
    }
    return NULL;
}
