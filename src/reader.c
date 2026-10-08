#include "app.h"
#include "ndef.h"
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
typedef struct {
    int fd, initialized, failed;
    unsigned opened;
    int trace,stream,rx_pending,wake_commands,acr,bounded_rx;
    uint8_t sequence;
    unsigned poll_traces;
    uint8_t rx_buffer[2048];
    size_t rx_size;
    uint8_t transfer_data[2][512];
    void *buffers[2][1];
    uint32_t lengths[2][1];
    struct usb_fs_endpoint ep[2];
    uint8_t in,out;
    uint16_t in_packet;
    unsigned rx_submissions,rx_completions;
    uint16_t vid,pid;
    char path[64];
} Reader;
static Reader reader={.fd=-1};
static int control(Reader *r,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t len) {
    struct usb_ctl_request q;
    memset(&q,0,sizeof(q));
    q.ucr_data=data;
    q.ucr_request.bmRequestType=type; q.ucr_request.bRequest=request;
    USETW(q.ucr_request.wValue,value); USETW(q.ucr_request.wIndex,index); USETW(q.ucr_request.wLength,len);
    q.ucr_flags=0x0004;
    if(ioctl(r->fd,USB_DO_REQUEST,&q)!=0) {
        fprintf(stderr,"[NFC] USB control failed type=%02x request=%02x value=%04x index=%04x errno=%d\n",type,request,value,index,errno);
        return -1;
    }
    return q.ucr_actlen;
}
static void disconnect_reader(void) {
    if(reader.fd>=0) {
        if(reader.initialized) {
            for(unsigned i=0;i<2;i++) if(reader.opened & (1u<<i)) {
                struct usb_fs_stop stop; memset(&stop,0,sizeof(stop)); stop.ep_index=i;
                ioctl(reader.fd,USB_FS_STOP,&stop);
                struct usb_fs_close endpoint; memset(&endpoint,0,sizeof(endpoint)); endpoint.ep_index=i;
                ioctl(reader.fd,USB_FS_CLOSE,&endpoint);
            }
            struct usb_fs_uninit u; memset(&u,0,sizeof(u));
            ioctl(reader.fd,USB_FS_UNINIT,&u);
        }
        close(reader.fd);
    }
    memset(&reader,0,sizeof(reader)); reader.fd=-1;
    pthread_mutex_lock(&state_lock); session_disconnect(&session); pthread_mutex_unlock(&state_lock);
}
static int identity(Reader *r,uint16_t *vid,uint16_t *pid) {
    struct usb_device_info info;
    memset(&info,0,sizeof(info));
    if(ioctl(r->fd,USB_GET_DEVICEINFO,&info)==0) { *vid=info.udi_vendorNo; *pid=info.udi_productNo; return 0; }
    struct usb_device_descriptor d;
    memset(&d,0,sizeof(d));
    if(ioctl(r->fd,USB_GET_DEVICE_DESC,&d)==0 && d.bLength==18) { *vid=UGETW(d.idVendor); *pid=UGETW(d.idProduct); return 0; }
    uint8_t raw[18]={0};
    if(control(r,0x80,6,0x100,0,raw,sizeof(raw))>=18 && raw[1]==1) {
        *vid=raw[8]|raw[9]<<8; *pid=raw[10]|raw[11]<<8; return 0;
    }
    return -1;
}
static int endpoints(Reader *r) {
    uint8_t desc[1024]={0};
    int n=control(r,0x80,6,0x200,0,desc,9);
    if(n<9 || desc[1]!=2) return -1;
    unsigned total=desc[2]|desc[3]<<8;
    if(total<9 || total>sizeof(desc)) return -1;
    n=control(r,0x80,6,0x200,0,desc,total);
    if(n<(int)total) return -1;
    int iface=-1,chosen=-1;
    for(unsigned i=0;i+2<=total;) {
        unsigned len=desc[i],type=desc[i+1];
        if(len<2 || i+len>total) return -1;
        if(type==4 && len>=9) iface=desc[i+3]==0 ? desc[i+2] : -1;
        if(type==5 && len>=7 && iface>=0 && (desc[i+3]&3)==2) {
            if(chosen<0) chosen=iface;
            if(chosen==iface) {
                if(desc[i+2]&0x80) {
                    r->in=desc[i+2]; r->in_packet=(desc[i+4]|desc[i+5]<<8)&0x7ff;
                } else r->out=desc[i+2];
            }
        }
        i+=len;
    }
    if(!r->in || !r->out || chosen<0) return -1;
    if(!r->in_packet || r->in_packet>sizeof(r->transfer_data[0])) return -1;
    ioctl(r->fd,USB_IFACE_DRIVER_DETACH,&chosen);
    return 0;
}
static void bridge_status(Reader *r,const char *phase) {
    uint8_t status[2]={0};
    int n=control(r,0xc0,0x95,0x0706,0,status,sizeof(status));
    if(n==2) fprintf(stderr,"[NFC] CH340 modem status %s raw=%02x %02x active=%02x\n",phase,status[0],status[1],(unsigned)(~status[0])&15);
    else fprintf(stderr,"[NFC] CH340 modem status %s unavailable count=%d\n",phase,n);
}
static int bridge_init(Reader *r) {
    uint8_t version[2]={0};
    if(control(r,0xc0,0x5f,0,0,version,2)<2) return -1;
    if(control(r,0x40,0xa1,0,0,NULL,0)<0) return -1;
    uint16_t divisor=0xcc03;
    if(version[0]>0x27) divisor|=0x80;
    if(control(r,0x40,0xa1,0xc39c,divisor,NULL,0)<0) return -1;
    fprintf(stderr,"[NFC] CH340 combined serial init value=c39c index=%04x\n",divisor);
    if(control(r,0x40,0x9a,0x1312,divisor,NULL,0)<0) return -1;
    fprintf(stderr,"[NFC] CH340 separate baud write value=1312 index=%04x (115200)\n",divisor);
    if(control(r,0x40,0x9a,0x0f2c,0x0007,NULL,0)<0) return -1;
    fprintf(stderr,"[NFC] CH340 upload timeout configured value=0f2c index=0007\n");
    if(control(r,0x40,0xa4,0xffff,0,NULL,0)<0) return -1;
    bridge_status(r,"before flow configuration");
    if(control(r,0x40,0x9a,0x2727,0,NULL,0)<0) return -1;
    const uint16_t registers[]={0x1312,0x2518,0x2727,0x0f2c};
    for(unsigned i=0;i<sizeof(registers)/sizeof(*registers);i++) {
        uint8_t values[2]={0};
        int count=control(r,0xc0,0x95,registers[i],0,values,sizeof(values));
        if(count==2) fprintf(stderr,"[NFC] CH340 register %04x readback=%02x %02x\n",registers[i],values[0],values[1]);
        else fprintf(stderr,"[NFC] CH340 register %04x readback unavailable count=%d\n",registers[i],count);
    }
    fprintf(stderr,"[NFC] CH340 version=0x%02x 115200 8N1 flow=none DTR=off RTS=off\n",version[0]);
    bridge_status(r,"configured");
    return 0;
}
static void trace_bytes(const char *label,const uint8_t *data,size_t size) {
    char hex[3*64+1]; size_t count=size>64?64:size;
    for(size_t i=0;i<count;i++) snprintf(hex+3*i,sizeof(hex)-3*i,"%02x ",data[i]);
    hex[3*count]=0;
    fprintf(stderr,"[NFC] %s bytes=%zu: %s%s\n",label,size,hex,size>count?"...":"");
}
static int queue_receive(Reader *r) {
    if(r->rx_pending) return 0;
    unsigned requested=r->acr?sizeof(r->transfer_data[0]):r->in_packet?r->in_packet:32;
    r->buffers[0][0]=r->transfer_data[0]; r->lengths[0][0]=requested;
    struct usb_fs_endpoint *ep=&r->ep[0];
    memset(ep,0,sizeof(*ep)); ep->ppBuffer=r->buffers[0]; ep->pLength=r->lengths[0];
    ep->nFrames=1; ep->timeout=0;
    ep->flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;
    struct usb_fs_start start; memset(&start,0,sizeof(start));
    if(ioctl(r->fd,USB_FS_START,&start)!=0) {
        fprintf(stderr,"[NFC] Queued RX start failed errno=%d\n",errno); r->failed=1; return -1;
    }
    r->rx_pending=1;
    r->rx_submissions++;
    if(!r->acr && r->trace) fprintf(stderr,"[NFC] RX submit=%u endpoint=%02x requested=%u buffered=%zu\n",r->rx_submissions,r->in,requested,r->rx_size);
    return 0;
}
static int stream_complete(Reader *r,struct usb_fs_complete *complete) {
    if(ioctl(r->fd,USB_FS_COMPLETE,complete)!=0) return -1;
    if(complete->ep_index==1) return 0;
    if(complete->ep_index!=0 || !r->rx_pending) { errno=EIO; r->failed=1; return -1; }
    r->rx_pending=0;
    struct usb_fs_endpoint *ep=&r->ep[0];
    size_t size=r->lengths[0][0];
    unsigned requested=r->acr?sizeof(r->transfer_data[0]):r->in_packet?r->in_packet:32;
    r->rx_completions++;
    if(!r->acr && r->trace) fprintf(stderr,"[NFC] RX complete=%u submissions=%u status=%d frames=%u length=%zu requested=%u\n",r->rx_completions,r->rx_submissions,ep->status,ep->aFrames,size,requested);
    if(ep->status!=0 || ep->aFrames!=1 || size>requested || size>sizeof(r->rx_buffer)-r->rx_size) {
        fprintf(stderr,"[NFC] Queued RX failed status=%d frames=%u length=%zu buffered=%zu\n",ep->status,ep->aFrames,size,r->rx_size);
        errno=EIO; r->failed=1; return -1;
    }
    if(r->trace) fprintf(stderr,"[NFC] Queued RX completion length=%zu\n",size);
    memcpy(r->rx_buffer+r->rx_size,r->transfer_data[0],size); r->rx_size+=size;
    if(queue_receive(r)!=0) return -1;
    errno=EBUSY;
    return -1;
}
static void stream_reset(Reader *r) {
    if(!r->stream) return;
    if(r->rx_pending) {
        struct usb_fs_stop stop; memset(&stop,0,sizeof(stop)); stop.ep_index=0;
        if(ioctl(r->fd,USB_FS_STOP,&stop)!=0) {
            fprintf(stderr,"[NFC] RX stop failed errno=%d\n",errno);
            r->failed=1; return;
        }
        uint64_t end=now_ms()+300;
        for(;;) {
            struct usb_fs_complete complete; memset(&complete,0,sizeof(complete));
            if(ioctl(r->fd,USB_FS_COMPLETE,&complete)==0) {
                if(complete.ep_index==0) break;
                fprintf(stderr,"[NFC] Unexpected endpoint during RX cancellation=%u\n",complete.ep_index);
                r->failed=1; return;
            }
            if(errno!=EBUSY || now_ms()>=end) {
                fprintf(stderr,"[NFC] RX cancellation completion failed errno=%d\n",errno);
                r->failed=1; return;
            }
            usleep(1000);
        }
        r->rx_pending=0;
    }
    r->rx_size=0;
}
static int receive_stream(Reader *r,uint8_t *data,uint32_t size,unsigned timeout) {
    if(queue_receive(r)!=0) return -1;
    uint64_t end=now_ms()+timeout;
    while(!r->rx_size) {
        struct usb_fs_complete complete; memset(&complete,0,sizeof(complete));
        int rc=stream_complete(r,&complete);
        if(rc==0 || (errno!=EBUSY) || r->failed) { r->failed=1; return -1; }
        if(r->rx_size) break;
        if(now_ms()>=end) return 0;
        usleep(1000);
    }
    size_t count=r->rx_size<size?r->rx_size:size;
    memcpy(data,r->rx_buffer,count); r->rx_size-=count;
    memmove(r->rx_buffer,r->rx_buffer+count,r->rx_size);
    return (int)count;
}
static int transfer(Reader *r,int input,uint8_t *data,uint32_t size,unsigned timeout) {
    unsigned idx=input ? 0 : 1;
    if(r->failed || size>sizeof(r->transfer_data[0])) return -1;
    if(input && r->bounded_rx) {
        unsigned packet=r->in_packet?r->in_packet:32;
        if(size>packet) size=packet;
    }
    if(r->stream) {
        if(input) return receive_stream(r,data,size,timeout);
        if(queue_receive(r)!=0) return -1;
    }
    r->buffers[idx][0]=r->transfer_data[idx]; r->lengths[idx][0]=size;
    if(!input) memcpy(r->transfer_data[idx],data,size);
    struct usb_fs_endpoint *ep=&r->ep[idx];
    memset(ep,0,sizeof(*ep)); ep->ppBuffer=r->buffers[idx]; ep->pLength=r->lengths[idx];
    ep->nFrames=1; ep->timeout=(uint16_t)timeout;
    if(input) ep->flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;
    struct usb_fs_start start; memset(&start,0,sizeof(start)); start.ep_index=idx;
    if(ioctl(r->fd,USB_FS_START,&start)!=0) { fprintf(stderr,"[NFC] USB START endpoint=%u errno=%d\n",idx,errno); r->failed=1; return -1; }
    if(input && r->bounded_rx) {
        r->rx_submissions++;
        if(r->trace) fprintf(stderr,"[NFC] Bounded RX submit=%u requested=%u timeout=%u\n",r->rx_submissions,size,timeout);
    }
    uint64_t end=now_ms()+timeout+300;
    for(;;) {
        struct usb_fs_complete complete; memset(&complete,0,sizeof(complete));
        if((r->stream?stream_complete(r,&complete):ioctl(r->fd,USB_FS_COMPLETE,&complete))==0) {
            if(input && r->bounded_rx) {
                r->rx_completions++;
                if(r->trace) fprintf(stderr,"[NFC] Bounded RX complete=%u endpoint=%u status=%d frames=%u length=%u\n",r->rx_completions,complete.ep_index,ep->status,ep->aFrames,r->lengths[idx][0]);
            }
            if(r->trace || (ep->status!=0 && ep->status!=20)) fprintf(stderr,"[NFC] USB done endpoint=%u returned=%u status=%d frames=%u length=%u requested=%u\n",idx,complete.ep_index,ep->status,ep->aFrames,r->lengths[idx][0],size);
            if(complete.ep_index!=idx) { r->failed=1; return -1; }
            if(ep->status==20) {
                if(!input) { r->failed=1; return -1; }
                return 0;
            }
            if(ep->status!=0 || ep->aFrames!=1 || r->lengths[idx][0]>size) { r->failed=1; return -1; }
            if(input) memcpy(data,r->transfer_data[idx],r->lengths[idx][0]);
            return (int)r->lengths[idx][0];
        }
        if(r->failed || errno!=EBUSY || now_ms()>=end) {
            fprintf(stderr,"[NFC] USB COMPLETE endpoint=%u errno=%d deadline=%d\n",idx,errno,now_ms()>=end);
            struct usb_fs_stop stop; memset(&stop,0,sizeof(stop)); stop.ep_index=idx;
            ioctl(r->fd,USB_FS_STOP,&stop);
            r->failed=1;
            if(r->bounded_rx) {
                uint64_t cancel_end=now_ms()+300;
                for(;;) {
                    struct usb_fs_complete cancelled; memset(&cancelled,0,sizeof(cancelled));
                    if(ioctl(r->fd,USB_FS_COMPLETE,&cancelled)==0) {
                        fprintf(stderr,"[NFC] Bounded transfer cancellation endpoint=%u completed=%u\n",idx,cancelled.ep_index);
                        break;
                    }
                    if(errno!=EBUSY || now_ms()>=cancel_end) {
                        fprintf(stderr,"[NFC] Bounded transfer cancellation drain failed errno=%d\n",errno); break;
                    }
                    usleep(1000);
                }
            }
            return -1;
        }
        usleep(1000);
    }
}
static int acr_exchange(Reader *r,uint8_t type,const uint8_t *payload,size_t length,uint8_t *out,size_t cap,unsigned timeout) {
    uint8_t tx[280]={0},rx[512]; size_t have=0;
    if(length>sizeof(tx)-10) return -1;
    uint8_t seq=r->sequence++;
    tx[0]=type; tx[1]=(uint8_t)length; tx[2]=(uint8_t)(length>>8); tx[6]=seq;
    if(type==0x62) tx[7]=1;
    if(length) memcpy(tx+10,payload,length);
    if(r->trace) trace_bytes("ACR122U CCID TX",tx,length+10);
    if(transfer(r,0,tx,(uint32_t)length+10,1000)!=(int)length+10) return -1;
    uint64_t end=now_ms()+timeout;
    while(now_ms()<end) {
        if(have>=10) {
            uint32_t n=(uint32_t)rx[1]|(uint32_t)rx[2]<<8|(uint32_t)rx[3]<<16|(uint32_t)rx[4]<<24;
            if(n>sizeof(rx)-10 || rx[0]!=0x80 || rx[5]!=0 || rx[6]!=seq) goto invalid;
            if(have>=10+n) {
                unsigned status=rx[7]&0xc0;
                if(status==0x80 && n==0) {
                    memmove(rx,rx+10,have-10); have-=10; continue;
                }
                if(r->trace || status!=0) fprintf(stderr,"[NFC] ACR122U CCID reply seq=%u status=%02x error=%02x length=%u\n",seq,rx[7],rx[8],n);
                if(type==0x6f && n==0 && rx[8]==0xfe && have==10) {
                    if(r->trace) fprintf(stderr,"[NFC] ACR122U ICC_MUTE timeout\n");
                    return -2;
                }
                if(type==0x62 && status==0x40 && have==10+n) {
                    fprintf(stderr,"[NFC] ACR122U power-on reports inactive ICC; checking direct firmware command\n"); return 0;
                }
                if(status!=0 || n>cap || have!=10+n) goto invalid;
                if(n) memcpy(out,rx+10,n);
                return (int)n;
            }
        }
        int n=transfer(r,1,rx+have,(uint32_t)(sizeof(rx)-have),200);
        if(n<0) return -1;
        if(n && r->trace) trace_bytes("ACR122U CCID RX",rx+have,(size_t)n);
        have+=(unsigned)n;
    }
    fprintf(stderr,"[NFC] ACR122U CCID deadline seq=%u received=%zu\n",seq,have);
    r->failed=1; return -1;
invalid:
    trace_bytes("ACR122U invalid CCID",rx,have); r->failed=1; return -1;
}
static int acr_command(Reader *r,uint8_t cmd,const uint8_t *data,size_t length,uint8_t *out,size_t cap,unsigned timeout) {
    uint8_t apdu[260]={0xff,0,0,0,0,0xd4},reply[280];
    if(length>253) return -1;
    apdu[4]=(uint8_t)(length+2); apdu[6]=cmd;
    if(length) memcpy(apdu+7,data,length);
    if(r->trace) fprintf(stderr,"[NFC] ACR122U PN532 command=%02x\n",cmd);
    int n=acr_exchange(r,0x6f,apdu,length+7,reply,sizeof(reply),timeout);
    if(n==2 && reply[0]==0x61) {
        uint8_t fetch[]={0xff,0xc0,0,0,reply[1]};
        n=acr_exchange(r,0x6f,fetch,sizeof(fetch),reply,sizeof(reply),timeout);
    }
    if(n==-2 && cmd==0x4a && cap>=1) { out[0]=0; return 1; }
    if(n<4 || reply[0]!=0xd5 || reply[1]!=(uint8_t)(cmd+1) || reply[n-2]!=0x90 || reply[n-1]!=0 || (size_t)n-4>cap) {
        if(n>=0) trace_bytes("ACR122U invalid APDU",reply,(size_t)n);
        return -1;
    }
    memcpy(out,reply+2,(size_t)n-4); return n-4;
}
static int acr_init(Reader *r) {
    uint8_t reply[280]; r->trace=1; r->stream=1;
    reader_message("ACR122U USB CCID initialization");
    if(acr_exchange(r,0x62,NULL,0,reply,sizeof(reply),2000)<0) return -1;
    int n=acr_command(r,2,NULL,0,reply,sizeof(reply),2000);
    if(n!=4 || reply[0]!=0x32) return -1;
    unsigned major=reply[1],minor=reply[2];
    const uint8_t normal[]={1},parameters[]={0x14};
    reader_message("ACR122U configuring normal reader mode");
    if(acr_command(r,0x14,normal,sizeof(normal),reply,sizeof(reply),3000)!=0) return -1;
    if(acr_command(r,0x12,parameters,sizeof(parameters),reply,sizeof(reply),2000)!=0) return -1;
    const uint8_t retries[]={5,0xff,1,1};
    if(acr_command(r,0x32,retries,sizeof(retries),reply,sizeof(reply),2000)!=0) return -1;
    r->trace=0; r->poll_traces=8;
    reader_message("ACR122U PN532 %u.%u ready via %s (%04x:%04x)",major,minor,r->path,r->vid,r->pid);
    return 0;
}
static int wake_command(Reader *r) {
    uint8_t wake[16]={0x55};
    if(r->trace) trace_bytes("Command wake TX",wake,sizeof(wake));
    if(transfer(r,0,wake,sizeof(wake),300)!=(int)sizeof(wake)) return -1;
    usleep(10000);
    return 0;
}
static int command_attempt(Reader *r,uint8_t cmd,const uint8_t *data,size_t len,uint8_t *out,size_t cap,unsigned timeout,int *ack) {
    if(r->acr) return acr_command(r,cmd,data,len,out,cap,timeout);
    uint8_t frame[264],buf[512]; size_t have=0;
    r->rx_size=0;
    if(r->wake_commands && wake_command(r)!=0) return -1;
    int n=pn532_frame(cmd,data,len,frame,sizeof(frame));
    if(n<0) return -1;
    if(r->trace) { fprintf(stderr,"[NFC] PN532 command=%02x\n",cmd); trace_bytes("TX",frame,(size_t)n); }
    if(transfer(r,0,frame,n,300)!=n) { fprintf(stderr,"[NFC] PN532 command=%02x transmit failed\n",cmd); return -1; }
    usleep((unsigned)((n*1000000ULL*10+115199)/115200)+2000);
    unsigned total=0;
    uint64_t end=now_ms()+timeout;
    while(now_ms()<end) {
        if(have) {
            size_t used=0;
            int rc=pn532_parse(buf,have,cmd,out,cap,&used);
            if(rc<0) { fprintf(stderr,"[NFC] PN532 command=%02x invalid response\n",cmd); trace_bytes("Invalid RX",buf,have); return -1; }
            if(rc==2) { *ack=1; if(r->trace) fprintf(stderr,"[NFC] PN532 command=%02x ACK received\n",cmd); }
            if(rc>=3) {
                if(r->trace) trace_bytes("Response payload",out,(size_t)(rc-3));
                return rc-3;
            }
            if(used) { memmove(buf,buf+used,have-used); have-=used; if(have) continue; }
        }
        if(have==sizeof(buf)) return -1;
        int got=transfer(r,1,buf+have,sizeof(buf)-have,200);
        if(got<0) { fprintf(stderr,"[NFC] PN532 command=%02x receive failed after %u bytes; ACK=%d\n",cmd,total,*ack); return -1; }
        if(got && r->trace) trace_bytes("RX",buf+have,(size_t)got);
        total+=(unsigned)got; have+=(unsigned)got;
    }
    fprintf(stderr,"[NFC] PN532 command=%02x response deadline; received=%u buffered=%zu ACK=%d\n",cmd,total,have,*ack);
    fprintf(stderr,"[NFC] RX deadline pending=%d submissions=%u completions=%u buffered=%zu packet=%u\n",r->rx_pending,r->rx_submissions,r->rx_completions,r->rx_size,r->in_packet?r->in_packet:32);
    return -1;
}
static int command_timed(Reader *r,uint8_t cmd,const uint8_t *data,size_t len,uint8_t *out,size_t cap,unsigned timeout) {
    if(r->acr) return acr_command(r,cmd,data,len,out,cap,timeout);
    for(unsigned attempt=0;attempt<3;attempt++) {
        int ack=0;
        int result=command_attempt(r,cmd,data,len,out,cap,timeout,&ack);
        if(result>=0 || r->failed || ack) return result;
        if(attempt<2) {
            fprintf(stderr,"[NFC] PN532 command=%02x retry after missing ACK attempt=%u/3\n",cmd,attempt+1);
            stream_reset(r);
            if(r->failed) return -1;
            usleep(attempt?100000:50000);
        }
    }
    return -1;
}
static int command(Reader *r,uint8_t cmd,const uint8_t *data,size_t len,uint8_t *out,size_t cap) {
    return command_timed(r,cmd,data,len,out,cap,1500);
}
static int configure_sam(Reader *r) {
    const uint8_t options[2][3]={{1,0x14,1},{1,1,0}};
    uint8_t reply[64];
    for(unsigned i=0;i<2;i++) {
        if(r->failed) return -1;
        fprintf(stderr,"[NFC] SAM attempt=%u timeout=3000 parameters=%02x %02x %02x\n",i+1,options[i][0],options[i][1],options[i][2]);
        int ack=0;
        if(command_attempt(r,0x14,options[i],sizeof(options[i]),reply,sizeof(reply),3000,&ack)==0) return 0;
        if(r->failed) return -1;
    }
    return -1;
}
static int __attribute__((unused)) verify_firmware_repeats(Reader *r,const uint8_t expected[4]) {
    uint8_t reply[64];
    fprintf(stderr,"[NFC] Queued transport diagnostic pid=%d: repeating firmware\n",(int)getpid());
    for(unsigned probe=0;probe<2;probe++) {
        if(r->failed) return -1;
        int n=command(r,2,NULL,0,reply,sizeof(reply));
        int valid=n==4 && !memcmp(reply,expected,4);
        fprintf(stderr,"[NFC] Firmware repeat=%u/2 valid=%d payload_length=%d USB_failed=%d\n",probe+1,valid,n,r->failed);
        if(!valid && !r->failed && !r->wake_commands) {
            fprintf(stderr,"[NFC] Same-session wake probe: no USB reset, reconnect or serial reconfiguration\n");
            r->wake_commands=1;
            n=command(r,2,NULL,0,reply,sizeof(reply));
            valid=n==4 && !memcmp(reply,expected,4);
            fprintf(stderr,"[NFC] Same-session wake result valid=%d payload_length=%d USB_failed=%d\n",valid,n,r->failed);
            if(valid) {
                for(unsigned check=0;check<2;check++) {
                    n=command(r,2,NULL,0,reply,sizeof(reply));
                    valid=n==4 && !memcmp(reply,expected,4);
                    fprintf(stderr,"[NFC] Wake confirmation=%u/2 valid=%d payload_length=%d\n",check+1,valid,n);
                    if(!valid) break;
                }
            }
            if(valid) { fprintf(stderr,"[NFC] Wake before each command enabled after three matching replies\n"); return 0; }
        }
        if(!valid) { reader_message("Firmware repeat and same-session wake probe failed; reconnecting."); return -1; }
    }
    fprintf(stderr,"[NFC] Firmware repeat test passed; proceeding to SAM\n");
    return 0;
}
static int pn_init(Reader *r) {
    uint8_t reply[64],wake[4][40]={{0}};
    const unsigned lengths[]={40,6,6,40};
    memset(wake[0],0x55,16); memset(wake[0]+32,0xff,4);
    wake[1][0]=0x55; wake[1][1]=0x55;
    const uint8_t ack[]={0,0,0xff,0,0xff,0}; memcpy(wake[2],ack,sizeof(ack));
    memset(wake[3],0x55,32);
    r->trace=1; r->stream=0; r->bounded_rx=1; r->wake_commands=0;
    fprintf(stderr,"[NFC] Bounded USB receive enabled; packet=%u fresh transfer per read, no automatic requeue\n",r->in_packet?r->in_packet:32);
    usleep(200000);
    for(unsigned i=0;i<4;i++) {
        trace_bytes("Companion wake TX",wake[i],lengths[i]);
        if(transfer(r,0,wake[i],lengths[i],300)!=(int)lengths[i]) return -1;
        usleep(150000);
    }
    reader_message("Companion wake sequence sent; querying PN532 firmware");
    int n=-1;
    for(unsigned attempt=0;attempt<3;attempt++) {
        fprintf(stderr,"[NFC] Firmware query attempt=%u/3\n",attempt+1);
        n=command(r,2,NULL,0,reply,sizeof(reply));
        if(n==4 && reply[0]==0x32) break;
        if(r->failed) break;
        usleep(150000);
    }
    if(n!=4 || reply[0]!=0x32) { reader_message("USB bridge found; PN532 firmware handshake failed after bounded retries"); return -1; }
    unsigned major=reply[1],minor=reply[2];
    reader_message("PN532 firmware responded; using captured Companion SAM sequence");
    if(configure_sam(r)!=0) {
        reader_message("PN532 SAM initialization failed; reconnecting"); return -1;
    }
    r->trace=0;
    reader_message("PN532 %u.%u ready via %s (%04x:%04x)",major,minor,r->path,r->vid,r->pid);
    return 0;
}
static int tag_exchange(const uint8_t *data,size_t len,uint8_t *out,size_t cap) {
    uint8_t request[32],reply[64];
    if(len>sizeof(request)-1) return -1;
    request[0]=1; memcpy(request+1,data,len);
    int raw=len==1 && data[0]==0x60;
    int n=command(&reader,raw?0x42:0x40,raw?data:request,raw?len:len+1,reply,sizeof(reply));
    if(n<1 || (reply[0]&0x3f)!=0 || (size_t)n-1>cap) {
        fprintf(stderr,"[NFC] Tag exchange command=%02x payload=%02x length=%d status=%d\n",raw?0x42:0x40,len?data[0]:0,n,n>=1?reply[0]:-1);
        return -1;
    }
    memcpy(out,reply+1,n-1); return n-1;
}
static int tag_read_page(unsigned page,uint8_t out[16]) {
    uint8_t req[]={0x30,(uint8_t)page};
    return tag_exchange(req,sizeof(req),out,16)==16 ? 0 : -1;
}
static int tag_same_uid(const char *uid) {
    uint8_t poll[]={1,0},reply[64];
    int n=command(&reader,0x4a,poll,sizeof(poll),reply,sizeof(reply));
    if(n<6 || reply[0]!=1 || reply[5]*2!=strlen(uid) || n<6+reply[5]) return 0;
    char found[UID_CAP]={0};
    for(int i=0;i<reply[5];i++) snprintf(found+2*i,3,"%02X",reply[6+i]);
    return !strcmp(found,uid);
}
static int tag_write_page(unsigned page,const uint8_t data[4],const char *uid) {
    if(!tag_same_uid(uid)) return -1;
    uint8_t req[6]={0xa2,(uint8_t)page},reply[8]; memcpy(req+2,data,4);
    int n=tag_exchange(req,sizeof(req),reply,sizeof(reply));
    return n==0 || (n==1 && (reply[0]&15)==0x0a) ? 0 : -1;
}
static int read_card_title(char title[TITLE_CAP]) {
    uint8_t data[128],first[16]; title[0]=0;
    if(tag_read_page(0,first)!=0 || first[12]!=0xe1 || (first[13]>>4)!=1 || (first[15]>>4)==15) return 0;
    unsigned bytes=(unsigned)first[14]*8;
    if(bytes>sizeof(data)) bytes=sizeof(data);
    if(bytes<32) return 0;
    for(unsigned i=0;i<bytes;i+=16) if(tag_read_page(4+i/4,data+i)!=0) return 0;
    int found=ndef_decode_title(data,bytes,title);
    fprintf(stderr,"[NFC] Card NDEF result=%d title=%s\n",found,found==1?title:"none");
    if(found!=1) trace_bytes("Unrecognized card NDEF prefix",data,bytes<32?bytes:32);
    return found;
}
static int write_card(const char *title,const char *uid,char *message,size_t cap) {
    uint8_t req[]={0x60},ver[16],meta[16],locks[16],old[16],encoded[NDEF_TITLE_SIZE];
    int n=tag_exchange(req,sizeof(req),ver,sizeof(ver));
    unsigned dynamic,auth;
    if(n!=8 || ver[1]!=4 || ver[2]!=4 || (ver[6]!=0x0f && ver[6]!=0x11 && ver[6]!=0x13)) {
        snprintf(message,cap,"Unsupported card. Writing supports NTAG213, NTAG215 and NTAG216."); return -1;
    }
    dynamic=ver[6]==0x0f?40:ver[6]==0x11?130:226;
    auth=dynamic+1;
    if(tag_read_page(0,meta)!=0 || tag_read_page(dynamic,locks)!=0 || tag_read_page(4,old)!=0) {
        snprintf(message,cap,"Cannot read card protection settings; nothing written"); return -1;
    }
    uint8_t cfg[16];
    if(tag_read_page(auth,cfg)!=0 || meta[12]!=0xe1 || (meta[13]>>4)!=1 || meta[15]!=0 || meta[10] || meta[11] || locks[0] || locks[1] || locks[2] || cfg[3]!=0xff) {
        snprintf(message,cap,"Card is locked, protected or not NDEF formatted; nothing written"); return -1;
    }
    unsigned p=0; while(p<16 && old[p]==0) p++;
    if(p<16 && old[p]!=3 && old[p]!=0xfe) {
        snprintf(message,cap,"Unsupported card memory layout; nothing written"); return -1;
    }
    if(ndef_encode_title(title,encoded)!=0) return -1;
    uint8_t staging[4]; memcpy(staging,encoded,4); staging[1]=0;
    if(tag_write_page(4,staging,uid)!=0) goto failed;
    for(unsigned i=4;i<sizeof(encoded);i+=4) if(tag_write_page(4+i/4,encoded+i,uid)!=0) goto failed;
    if(tag_write_page(4,encoded,uid)!=0) goto failed;
    for(unsigned i=0;i<sizeof(encoded);i+=16) {
        uint8_t check[16]; unsigned count=sizeof(encoded)-i; if(count>16) count=16;
        if(!tag_same_uid(uid) || tag_read_page(4+i/4,check)!=0 || memcmp(check,encoded+i,count)) goto failed;
    }
    snprintf(message,cap,"Wrote %s to card %s and verified it. Remove and scan to launch.",title,uid);
    return 0;
failed:
    snprintf(message,cap,"Card write or verification failed. Keep the same card on the reader and retry; its previous NFC contents may have changed.");
    return -1;
}
static int open_candidate(const char *path,char *inventory,size_t cap) {
    Reader r; memset(&r,0,sizeof(r)); r.fd=open(path,O_RDWR);
    size_t used=strlen(inventory);
    if(r.fd<0) {
        if(errno!=ENOENT && used+90<cap) snprintf(inventory+used,cap-used,"%s: open errno %d; ",path,errno);
        return -1;
    }
    if(identity(&r,&r.vid,&r.pid)) {
        if(used+90<cap) snprintf(inventory+used,cap-used,"%s: descriptor errno %d; ",path,errno);
        close(r.fd); return -1;
    }
    if(used+90<cap) snprintf(inventory+used,cap-used,"%s %04x:%04x; ",path,r.vid,r.pid);
    r.acr=r.vid==0x072f && r.pid==0x2200;
    if(!r.acr && (r.vid!=0x1a86 || (r.pid!=0x7523 && r.pid!=0x5523))) { close(r.fd); return -1; }
    reader_message("%s detected at %s; discovering endpoints",r.acr?"ACR122U":"CH340",path);
    snprintf(r.path,sizeof(r.path),"%s",path);
    if(endpoints(&r)) { reader_message("%s found, but USB endpoint discovery failed (errno %d)",r.acr?"ACR122U":"CH340",errno); close(r.fd); return -1; }
    reader_message("%s endpoints %02x/%02x",r.acr?"ACR122U":"CH340",r.in,r.out);
    if(!r.acr && bridge_init(&r)) { reader_message("CH340 found, but serial configuration failed (errno %d)",errno); close(r.fd); return -1; }
    reader=r;
    reader_message("Initializing %s USBFS",reader.acr?"ACR122U":"CH340");
    struct usb_fs_init init; memset(&init,0,sizeof(init)); init.pEndpoints=reader.ep; init.ep_index_max=2;
    if(ioctl(reader.fd,USB_FS_INIT,&init)!=0) {
        reader_message("USB transfer initialization failed (errno %d)",errno); disconnect_reader(); return -1;
    }
    reader.initialized=1;
    for(int i=0;i<2;i++) {
        struct usb_fs_open o; memset(&o,0,sizeof(o)); o.ep_index=i; o.ep_no=i==0 ? reader.in : reader.out;
        o.max_bufsize=512; o.max_frames=1;
        if(ioctl(reader.fd,USB_FS_OPEN,&o)!=0) {
            reader_message("USB endpoint %02x open failed (errno %d)",o.ep_no,errno); disconnect_reader(); return -1;
        }
        reader.opened|=1u<<i;
    }
    reader_message("USBFS endpoints ready; starting PN532 handshake");
    if(reader.acr?acr_init(&reader):pn_init(&reader)) { reader_message("NFC reader initialization failed; reconnecting"); disconnect_reader(); return -1; }
    reader_message("%s ready; polling for cards",reader.acr?"ACR122U":"PN532");
    return 0;
}
static void discover(void) {
    char inv[2048]={0},paths[128][64]; int count=0;
    DIR *dir=opendir("/dev");
    if(dir) {
        struct dirent *e;
        while((e=readdir(dir)) && count<128) {
            unsigned bus,device; char tail;
            if(sscanf(e->d_name,"ugen%u.%u%c",&bus,&device,&tail)==2)
                snprintf(paths[count++],64,"/dev/ugen%u.%u",bus,device);
        }
        closedir(dir);
    }
    if(!count) {
        for(int bus=0;bus<4;bus++) for(int device=1;device<=24;device++)
            snprintf(paths[count++],64,"/dev/ugen%d.%d",bus,device);
    }
    for(int i=0;i<count;i++) if(open_candidate(paths[i],inv,sizeof(inv))==0) break;
    pthread_mutex_lock(&state_lock);
    snprintf(usb_inventory,sizeof(usb_inventory),"%s",inv[0]?inv:"No accessible USB devices found");
    pthread_mutex_unlock(&state_lock);
}
void *reader_main(void *arg) {
    (void)arg;
    for(;;) {
        process_action();
        if(reader.fd<0) { discover(); if(reader.fd<0) { usleep(2000000); continue; } }
        uint8_t poll[]={1,0},reply[128];
        if(reader.acr) reader.trace=reader.poll_traces>0;
        int n=command(&reader,0x4a,poll,sizeof(poll),reply,sizeof(reply));
        if(reader.acr && reader.poll_traces) {
            fprintf(stderr,"[NFC] ACR122U poll result length=%d targets=%d\n",n,n>=1?reply[0]:-1);
            reader.poll_traces--; reader.trace=0;
        }
        if(n<1 || reply[0]>1) {
            reader_message("Reader communication lost; game left running. Reconnecting...");
            disconnect_reader(); usleep(1000000); continue;
        }
        char uid[UID_CAP]={0};
        if(reply[0]==1) {
            if(n<6 || (reply[5]!=4 && reply[5]!=7 && reply[5]!=10) || n<6+reply[5]) {
                reader_message("Invalid NFC target response; game left running"); disconnect_reader(); continue;
            }
            for(int i=0;i<reply[5];i++) snprintf(uid+2*i,3,"%02X",reply[6+i]);
        }
        Config c; Session s;
        pthread_mutex_lock(&state_lock); c=config; s=session; pthread_mutex_unlock(&state_lock);
        char write_to[TITLE_CAP];
        if(get_write_request(write_to)) {
            if(uid[0]) {
                pthread_mutex_lock(&state_lock); operation_busy=1; pthread_mutex_unlock(&state_lock);
                if(!get_write_request(write_to)) {
                    pthread_mutex_lock(&state_lock); operation_busy=0; pthread_mutex_unlock(&state_lock); continue;
                }
                char message[256]; int wrote=write_card(write_to,uid,message,sizeof(message));
                finish_write(message);
                session_forget(&s); strcpy(s.present,uid); strcpy(s.last_uid,uid); s.reader_ready=1;
                pthread_mutex_lock(&state_lock);
                snprintf(card_title,sizeof(card_title),"%s",wrote==0?write_to:""); session=s; operation_busy=0;
                pthread_mutex_unlock(&state_lock);
            }
            usleep(100000); continue;
        }
        if(reader.acr && strcmp(s.present,uid)) fprintf(stderr,"[NFC] ACR122U card %s\n",uid[0]?uid:"removed");
        if(uid[0] && strcmp(s.present,uid) && strcmp(s.blocked_uid,uid)) {
            char embedded[TITLE_CAP]; int found=read_card_title(embedded);
            pthread_mutex_lock(&state_lock);
            snprintf(card_title,sizeof(card_title),"%s",found==1?embedded:"");
            pthread_mutex_unlock(&state_lock);
            int mapped=0; for(int i=0;i<c.count;i++) if(!strcmp(c.maps[i].uid,uid)) mapped=1;
            if(found==1 && !mapped) {
                int index=c.count<MAX_MAPS?c.count++:MAX_MAPS-1;
                Mapping *m=&c.maps[index]; memset(m,0,sizeof(*m)); strcpy(m->uid,uid); strcpy(m->title,embedded);
            }
        }
        if(get_write_request(write_to)) continue;
        pthread_mutex_lock(&state_lock); operation_busy=1; pthread_mutex_unlock(&state_lock);
        if(get_write_request(write_to)) {
            pthread_mutex_lock(&state_lock); operation_busy=0; pthread_mutex_unlock(&state_lock); continue;
        }
        session_scan(&s,&c,uid,now_ms(),sony_launch,sony_close);
        pthread_mutex_lock(&state_lock);
        session=s; operation_busy=0; if(!s.present[0]) card_title[0]=0;
        pthread_mutex_unlock(&state_lock);
        usleep(100000);
    }
    return NULL;
}
