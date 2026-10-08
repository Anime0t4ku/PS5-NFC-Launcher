#include "app.h"
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
Config config;
Session session;
char reader_status[256]="Searching for PN532 USB reader...",action_status[256]="Ready",usb_inventory[2048]="Scanning...";
char card_title[TITLE_CAP],write_status[256]="Choose a game and click Write to NFC card";
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static char pending_title[TITLE_CAP];
static int pending_close,pending;
int operation_busy;
static char write_title[TITLE_CAP];
static uint64_t write_deadline;
int arm_write(const char *title) {
    pthread_mutex_lock(&queue_lock);
    pthread_mutex_lock(&state_lock);
    if(operation_busy || (title && title[0] && (pending || (session.session_hold && session.owned_app)))) {
        pthread_mutex_unlock(&state_lock); pthread_mutex_unlock(&queue_lock); return -1;
    }
    snprintf(write_title,sizeof(write_title),"%s",title?title:"");
    write_deadline=title && title[0] ? now_ms()+120000 : 0;
    snprintf(write_status,sizeof(write_status),"%s",write_deadline?"Waiting for card. Place an unlocked NTAG213/215/216 on the reader.":"Card writing cancelled");
    pthread_mutex_unlock(&state_lock); pthread_mutex_unlock(&queue_lock); return 0;
}
int get_write_request(char title[TITLE_CAP]) {
    pthread_mutex_lock(&state_lock);
    if(write_deadline && now_ms()>write_deadline) {
        write_deadline=0; write_title[0]=0;
        snprintf(write_status,sizeof(write_status),"Card write timed out; arm writing again");
    }
    strcpy(title,write_title);
    int armed=write_deadline!=0;
    pthread_mutex_unlock(&state_lock); return armed;
}
void finish_write(const char *message) {
    fprintf(stderr,"[NFC] %s\n",message);
    pthread_mutex_lock(&state_lock);
    write_title[0]=0; write_deadline=0;
    snprintf(write_status,sizeof(write_status),"%s",message);
    pthread_mutex_unlock(&state_lock);
}
static void status_message(char status[256],const char *fmt,va_list ap) {
    char message[256];
    vsnprintf(message,sizeof(message),fmt,ap);
    pthread_mutex_lock(&state_lock);
    snprintf(status,256,"%s",message);
    pthread_mutex_unlock(&state_lock);
    fprintf(stderr,"[NFC] %s\n",message);
}
void action_message(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); status_message(action_status,fmt,ap); va_end(ap);
}
void reader_message(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); status_message(reader_status,fmt,ap); va_end(ap);
}
static void detach_stdio(void) {
    signal(SIGPIPE,SIG_IGN);
    int fd=open("/dev/null",O_RDWR);
    if(fd>=0) { for(int i=0;i<3;i++) { if(dup2(fd,i)<0) close(i); } if(fd>2) close(fd); }
    else { for(int i=0;i<3;i++) close(i); }
}
static void open_log(void) {
    mkdir("/data/ps5-nfc-launcher",0755);
    struct stat st;
    const char *path="/data/ps5-nfc-launcher/diagnostic.log";
    if(stat(path,&st)==0 && st.st_size>512*1024) rename(path,"/data/ps5-nfc-launcher/diagnostic.previous.log");
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0644);
    if(fd>=0) { dup2(fd,1); dup2(fd,2); if(fd>2) close(fd); }
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"\n[NFC] PS5 NFC Launcher v%s started\n",VERSION);
}
int enqueue_action(const char *title,int close_owned) {
    pthread_mutex_lock(&queue_lock);
    pthread_mutex_lock(&state_lock);
    int busy=operation_busy || write_deadline!=0;
    pthread_mutex_unlock(&state_lock);
    if(pending || busy) { pthread_mutex_unlock(&queue_lock); return -1; }
    pending=1; pending_close=close_owned;
    snprintf(pending_title,sizeof(pending_title),"%s",title?title:"");
    pthread_mutex_unlock(&queue_lock); return 0;
}
void process_action(void) {
    char title[TITLE_CAP]; int close_owned;
    pthread_mutex_lock(&queue_lock);
    if(!pending) { pthread_mutex_unlock(&queue_lock); return; }
    strcpy(title,pending_title); close_owned=pending_close;
    Session s;
    pthread_mutex_lock(&state_lock); s=session; operation_busy=1; pthread_mutex_unlock(&state_lock);
    pending=0; pthread_mutex_unlock(&queue_lock);
    if(close_owned) {
        if(!s.owned_app) action_message("No game owned by this payload to close");
        else if(sony_close(s.owned_app)==0) session_forget(&s);
    } else {
        if(s.session_hold && s.owned_app && sony_close(s.owned_app)!=0) {
            pthread_mutex_lock(&state_lock); operation_busy=0; pthread_mutex_unlock(&state_lock); return;
        }
        session_forget(&s);
        unsigned app=0;
        if(sony_launch(title,&app)==0 && app) { s.owned_app=app; s.owned_manual=1; strcpy(s.owned_title,title); }
    }
    pthread_mutex_lock(&state_lock); session=s; operation_busy=0; pthread_mutex_unlock(&state_lock);
}
int config_save(const Config *c) {
    if(mkdir("/data/ps5-nfc-launcher",0755)!=0 && errno!=EEXIST) return -1;
    int fd=open("/data/ps5-nfc-launcher/config.tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0) return -1;
    FILE *f=fdopen(fd,"w"); if(!f) { close(fd); return -1; }
    fprintf(f,"mode\t%s\nremoval_ms\t%u\n",c->hold?"hold":"tap",c->removal_ms);
    for(int i=0;i<c->count;i++) fprintf(f,"tag\t%s\t%s\t%s\n",c->maps[i].uid,c->maps[i].title,c->maps[i].name);
    int rc=0;
    if(fflush(f)!=0 || fsync(fd)!=0) rc=-1;
    if(fclose(f)!=0) rc=-1;
    if(rc==0 && rename("/data/ps5-nfc-launcher/config.tmp","/data/ps5-nfc-launcher/config.tsv")!=0) rc=-1;
    return rc;
}
void config_load(void) {
    memset(&config,0,sizeof(config)); config.removal_ms=500;
    FILE *f=fopen("/data/ps5-nfc-launcher/config.tsv","r"); if(!f) return;
    char line[256];
    while(fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        if(!strncmp(line,"mode\t",5)) config.hold=!strcmp(line+5,"hold");
        else if(!strncmp(line,"removal_ms\t",11)) {
            unsigned t=(unsigned)strtoul(line+11,NULL,10);
            if(t>=250 && t<=5000) config.removal_ms=t;
        } else if(!strncmp(line,"tag\t",4) && config.count<MAX_MAPS) {
            char *uid=line+4,*title=strchr(uid,'\t'); if(!title) continue; *title++=0;
            char *name=strchr(title,'\t'); if(!name) continue; *name++=0;
            if(!valid_uid(uid) || !valid_title(title) || strlen(name)>=NAME_CAP) continue;
            int duplicate=0;
            for(int i=0;i<config.count;i++) if(!strcmp(config.maps[i].uid,uid)) duplicate=1;
            if(duplicate) continue;
            Mapping *m=&config.maps[config.count++];
            strcpy(m->uid,uid); strcpy(m->title,title); strcpy(m->name,name);
        }
    }
    fclose(f);
}
static int credentials(void) {
    uint8_t caps[16]; memset(caps,0xff,sizeof(caps));
    int rc=0;
    rc|=kernel_set_ucred_uid(-1,0); rc|=kernel_set_ucred_ruid(-1,0); rc|=kernel_set_ucred_svuid(-1,0);
    rc|=kernel_set_ucred_rgid(-1,0); rc|=kernel_set_ucred_svgid(-1,0);
    rc|=kernel_set_ucred_caps(-1,caps);
    rc|=kernel_set_ucred_authid(-1,0x4800000000000006ULL);
    intptr_t root=kernel_get_root_vnode();
    if(!root) return -1;
    rc|=kernel_set_proc_rootdir(-1,root); rc|=kernel_set_proc_jaildir(-1,root);
    return rc;
}
static void fatal_signal(int sig) {
    const char *message="[NFC] Fatal signal: other\n";
    if(sig==SIGSEGV) message="[NFC] Fatal signal: SIGSEGV\n";
    else if(sig==SIGBUS) message="[NFC] Fatal signal: SIGBUS\n";
    else if(sig==SIGABRT) message="[NFC] Fatal signal: SIGABRT\n";
    else if(sig==SIGILL) message="[NFC] Fatal signal: SIGILL\n";
    else if(sig==SIGTERM) message="[NFC] Fatal signal: SIGTERM\n";
    size_t len=0; while(message[len]) len++;
    ssize_t ignored=write(STDERR_FILENO,message,len); (void)ignored;
    _exit(128+sig);
}
static int install_crash_log(void) {
    stack_t stack; memset(&stack,0,sizeof(stack));
    stack.ss_sp=malloc(65536); stack.ss_size=65536;
    if(!stack.ss_sp) return -1;
    if(sigaltstack(&stack,NULL)!=0) { free(stack.ss_sp); return -1; }
    struct sigaction action; memset(&action,0,sizeof(action));
    sigemptyset(&action.sa_mask); action.sa_handler=fatal_signal; action.sa_flags=SA_ONSTACK;
    const int signals[]={SIGSEGV,SIGBUS,SIGABRT,SIGILL,SIGTERM};
    for(unsigned i=0;i<sizeof(signals)/sizeof(*signals);i++) if(sigaction(signals[i],&action,NULL)!=0) return -1;
    return 0;
}
static void *reader_worker(void *arg) {
    if(install_crash_log()!=0) fprintf(stderr,"[NFC] Reader alternate signal stack unavailable\n");
    return reader_main(arg);
}
int main(void) {
    detach_stdio();
    if(credentials()!=0) { fprintf(stderr,"[NFC] Required payload credentials unavailable\n"); return 1; }
    open_log();
    if(install_crash_log()!=0) fprintf(stderr,"[NFC] Main alternate signal stack unavailable\n");
    fprintf(stderr,"[NFC] Credentials ready; pid=%d firmware=0x%08x\n",(int)getpid(),kernel_get_fw_version());
    if(http_prepare()!=0) return 1;
    config_load(); sony_init();
    pthread_attr_t attr;
    int rc=pthread_attr_init(&attr);
    if(rc) { fprintf(stderr,"[NFC] Thread attributes failed: %d\n",rc); return 1; }
    rc=pthread_attr_setstacksize(&attr,1024*1024);
    pthread_t http_thread,reader_thread;
    if(!rc) rc=pthread_create(&http_thread,&attr,http_main,NULL);
    if(rc) { fprintf(stderr,"[NFC] HTTP thread failed: %d\n",rc); pthread_attr_destroy(&attr); return 1; }
    pthread_detach(http_thread);
    fprintf(stderr,"[NFC] HTTP server started; reader worker starting\n");
    rc=pthread_create(&reader_thread,&attr,reader_worker,NULL);
    pthread_attr_destroy(&attr);
    if(rc) {
        reader_message("Reader worker could not start (%d); diagnostics remain available",rc);
        for(;;) pause();
    }
    pthread_join(reader_thread,NULL);
    reader_message("Reader worker stopped; diagnostics remain available");
    for(;;) pause();
}
