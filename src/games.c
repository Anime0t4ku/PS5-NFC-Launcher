#include "games.h"
#include "cJSON.h"
#include "appdb_scan.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef FILE_ROOT
#define FILE_ROOT ""
#endif
static const char *roots[]={FILE_ROOT "/user/appmeta",FILE_ROOT "/system_data/priv/appmeta"};
static uint32_t le32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int sfo_name(const char *path,char *out,size_t cap) {
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    uint8_t *data=malloc(65536); if(!data) { fclose(f); return -1; }
    size_t n=fread(data,1,65536,f); fclose(f);
    int result=-1;
    if(n<20 || le32(data)!=0x46535000) goto done;
    uint32_t key=le32(data+8),val=le32(data+12),count=le32(data+16);
    if(count>(n-20)/16 || key>=n || val>=n) goto done;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *e=data+20+i*16;
        size_t k=(size_t)key+(e[0]|e[1]<<8),v=(size_t)val+le32(e+12),len=le32(e+4);
        if(k+6>n || memcmp(data+k,"TITLE\0",6) || v>=n || len>n-v) continue;
        if(len>=cap) len=cap-1;
        memcpy(out,data+v,len); out[len]=0; result=out[0]?0:-1; break;
    }
done:
    free(data); return result;
}
static const char *find_title(cJSON *obj) {
    if(!obj) return NULL;
    cJSON *name=cJSON_GetObjectItemCaseSensitive(obj,"titleName");
    if(cJSON_IsString(name) && name->valuestring[0]) return name->valuestring;
    cJSON *child;
    cJSON_ArrayForEach(child,obj) {
        if(cJSON_IsObject(child) || cJSON_IsArray(child)) {
            const char *s=find_title(child); if(s) return s;
        }
    }
    return NULL;
}
static int json_name(const char *path,char *out,size_t cap) {
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    char *data=malloc(65537); if(!data) { fclose(f); return -1; }
    size_t n=fread(data,1,65536,f); fclose(f); data[n]=0;
    cJSON *json=cJSON_ParseWithLength(data,n+1); free(data); if(!json) return -1;
    cJSON *localized=cJSON_GetObjectItemCaseSensitive(json,"localizedParameters");
    cJSON *lang=cJSON_GetObjectItemCaseSensitive(localized,"defaultLanguage");
    cJSON *preferred=cJSON_IsString(lang)?cJSON_GetObjectItemCaseSensitive(localized,lang->valuestring):NULL;
    const char *s=find_title(preferred); if(!s) s=find_title(json);
    if(s) snprintf(out,cap,"%s",s);
    cJSON_Delete(json); return s?0:-1;
}
int game_icon(const char *title,char *path,size_t cap) {
    if(!valid_title(title)) return -1;
    for(unsigned i=0;i<sizeof(roots)/sizeof(*roots);i++) {
        struct stat st;
        snprintf(path,cap,"%s/%s/icon0.png",roots[i],title);
        if(stat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<8*1024*1024) return 0;
    }
    return -1;
}
static int metadata_name(const char *title,char *out,size_t cap) {
    char path[256];
    for(unsigned i=0;i<sizeof(roots)/sizeof(*roots);i++) {
        snprintf(path,sizeof(path),"%s/%s/param.json",roots[i],title);
        if(json_name(path,out,cap)==0) return 0;
        snprintf(path,sizeof(path),"%s/%s/param.sfo",roots[i],title);
        if(sfo_name(path,out,cap)==0) return 0;
    }
    return -1;
}
static int compare_game(const void *a,const void *b) { return strcasecmp(((const Game *)a)->name,((const Game *)b)->name); }
int games_list(Game *games,int max,char *source,size_t cap) {
    int count=0;
    FILE *f=fopen(FILE_ROOT "/system_data/priv/mms/app.db","rb");
    if(f) {
        struct stat st;
        if(fstat(fileno(f),&st)==0 && st.st_size>=100 && st.st_size<=64*1024*1024) {
            uint8_t *data=malloc((size_t)st.st_size);
            appdb_entry_t *entries=calloc((size_t)max,sizeof(*entries));
            if(data && entries && fread(data,1,(size_t)st.st_size,f)==(size_t)st.st_size) {
                int n=appdb_scan_entries_ex(data,(size_t)st.st_size,entries,max,1);
                for(int i=0;i<n && count<max;i++) {
                    if(!valid_title(entries[i].title_id)) continue;
                    Game *g=&games[count++]; memset(g,0,sizeof(*g));
                    strcpy(g->title,entries[i].title_id);
                    snprintf(g->name,sizeof(g->name),"%s",entries[i].name[0]?entries[i].name:g->title);
                    metadata_name(g->title,g->name,sizeof(g->name));
                    char path[256]; g->icon=game_icon(g->title,path,sizeof(path))==0;
                }
            }
            free(data); free(entries);
        }
        fclose(f);
    }
    if(count) snprintf(source,cap,"PS5 registered-game database");
    else {
        snprintf(source,cap,"Game metadata folders (database unavailable or empty)");
        for(unsigned i=0;i<sizeof(roots)/sizeof(*roots);i++) {
            DIR *d=opendir(roots[i]); if(!d) continue;
            struct dirent *e;
            while(count<max && (e=readdir(d))) {
                if(!valid_title(e->d_name)) continue;
                int duplicate=0;
                for(int j=0;j<count;j++) if(!strcmp(games[j].title,e->d_name)) duplicate=1;
                if(duplicate) continue;
                Game *g=&games[count++]; memset(g,0,sizeof(*g)); strcpy(g->title,e->d_name);
                strcpy(g->name,g->title); metadata_name(g->title,g->name,sizeof(g->name));
                char path[256]; g->icon=game_icon(g->title,path,sizeof(path))==0;
            }
            closedir(d);
        }
    }
    qsort(games,(size_t)count,sizeof(*games),compare_game);
    return count;
}
