#include "app.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef int (*Launch)(const char *, const char **, void *);
typedef int (*GetUser)(int *);
typedef int (*Init)(void *);
typedef int (*NoArg)(void);
typedef int (*Kill)(unsigned);
typedef struct { uint32_t size,user,opt; uint64_t crash; uint32_t check; } LaunchContext;
static Launch launch_sys;
static GetUser foreground_user;
static NoArg get_big_app, init_lnc;
static Init init_user;
static Kill kill_app,kill_lnc,kill_sys,kill_force;
int launch_available,close_available;
int sony_init(void) {
    launch_sys=(Launch)dlsym(RTLD_DEFAULT,"sceSystemServiceLaunchApp");
    foreground_user=(GetUser)dlsym(RTLD_DEFAULT,"sceUserServiceGetForegroundUser");
    get_big_app=(NoArg)dlsym(RTLD_DEFAULT,"sceSystemServiceGetAppIdOfRunningBigApp");
    init_user=(Init)dlsym(RTLD_DEFAULT,"sceUserServiceInitialize");
    init_lnc=(NoArg)dlsym(RTLD_DEFAULT,"sceLncUtilInitialize");
    kill_lnc=(Kill)dlsym(RTLD_DEFAULT,"sceLncUtilKillApp");
    kill_sys=(Kill)dlsym(RTLD_DEFAULT,"sceSystemServiceKillApp");
    kill_force=(Kill)dlsym(RTLD_DEFAULT,"sceLncUtilForceKillApp");
    void *h=dlopen("libSceSysCore.sprx",RTLD_LAZY);
    if(h) kill_app=(Kill)dlsym(h,"sceApplicationKill");
    if(init_user) init_user(NULL);
    if(init_lnc) init_lnc();
    launch_available=launch_sys!=NULL;
    close_available=kill_app || kill_lnc || kill_sys;
    fprintf(stderr,"[NFC] APIs: launch=%d close=%d running-app=%d\n",launch_available,close_available,get_big_app!=NULL);
    return launch_available ? 0 : -1;
}
int sony_launch(const char *title,unsigned *app) {
    *app=0;
    if(!valid_title(title) || !launch_sys) { action_message("Game launch API unavailable or title ID invalid"); return -1; }
    int before=get_big_app ? get_big_app() : -1;
    int user=-1;
    if(init_user) init_user(NULL);
    if(init_lnc) init_lnc();
    if(!foreground_user || foreground_user(&user)!=0 || user<=0) {
        action_message("Select a user profile on the PS5 before launching"); return -1;
    }
    LaunchContext ctx;
    memset(&ctx,0,sizeof(ctx)); ctx.size=sizeof(ctx); ctx.user=(uint32_t)user;
    const char *args[]={NULL};
    int rc=launch_sys(title,args,&ctx);
    usleep(50000);
    fprintf(stderr,"[NFC] launch %s rc=0x%08x before=%d\n",title,(unsigned)rc,before);
    if(rc<0) { action_message("Launch %s failed: 0x%08x",title,(unsigned)rc); return -1; }
    if(rc>0 && rc!=before) *app=(unsigned)rc;
    if(*app) action_message("Launched %s (app %u)",title,*app);
    else action_message("Launch accepted for %s; ownership unavailable, automatic close disabled",title);
    return 0;
}
int sony_close(unsigned app) {
    if(!app) return -1;
    int current=get_big_app ? get_big_app() : -1;
    if(current>=0 && (unsigned)current!=app) {
        action_message("Tracked game is no longer the running game; left current game alone"); return 0;
    }
    int rc=-1;
    if(kill_app) rc=kill_app(app);
    if(rc!=0 && kill_lnc) rc=kill_lnc(app);
    if(rc!=0 && kill_sys) rc=kill_sys(app);
    if(rc!=0 && kill_force) rc=kill_force(app);
    usleep(50000);
    fprintf(stderr,"[NFC] close app=%u rc=0x%08x\n",app,(unsigned)rc);
    if(rc==0) action_message("Closed tracked game (app %u)",app);
    else action_message("Close app %u failed: 0x%08x",app,(unsigned)rc);
    return rc==0 ? 0 : -1;
}
