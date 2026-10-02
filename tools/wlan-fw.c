/* Minimal WLFW board-data client for the vendor AF_MSM_IPC transport.
 * Wire tags are from kernel/drivers/soc/qcom/wlan_firmware_service_v01.c.
 * Does not start/reset the modem, own GPS helpers, or write calibration.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "msmipc.h"
#include "qmux.h"
#define SERVICE 0x45
#define CHUNK 6144
#define LIMIT (1024 * 1024)
static int fd, msa, ready;
static uint32_t node, port;
static uint16_t transaction;
static uint8_t reply[8192];
static size_t reply_len;
static uint16_t result_code, error_code;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2)<<16; }
static void put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void die(const char *s) { fprintf(stderr,"wlan-fw: %s\n",s); exit(1); }
static int64_t now(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) die("clock"); return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void add(uint8_t *b, size_t *n, uint8_t tag, const void *v, size_t len) {
    ssize_t r=tlv_put_raw(b+*n,8192-*n,tag,v,len);
    if(r<0) die("TLV overflow");
    *n+=(size_t)r;
}
static void add32(uint8_t *b,size_t *n,uint8_t tag,uint32_t v) { uint8_t x[4]; put32(x,v); add(b,n,tag,x,4); }
static int valid_tlvs(const uint8_t *b,size_t len) {
    size_t off=0;
    while(off<len) {
        if(len-off<3) return 0;
        size_t n=le16(b+off+1); off+=3;
        if(n>len-off) return 0;
        off+=n;
    }
    return 1;
}
/* Consume only the selected peer; preserve indications while awaiting replies. */
static int receive(int64_t deadline, uint16_t txn, uint16_t msg) {
    uint8_t b[8192]; uint32_t from_node,from_port;
    for (;;) {
        int64_t left=deadline-now(); if(left<=0) return 0;
        struct pollfd p={fd,POLLIN,0};
        int r=poll(&p,1,(int)left); if(r<0 && errno==EINTR) continue;
        if(r<0) die("poll failed");
        if(!r) return 0;
        if(!(p.revents&POLLIN)) die("transport disconnected");
        r=qrtr_recvfrom(fd,b,sizeof(b),&from_node,&from_port);
        if(r==QRTR_RECV_RESUME_TX) continue;
        if(r<0) { if(r==-EINTR) continue; die("receive failed"); }
        if(from_node!=node || from_port!=port) continue;
        struct qmi_sdu_header h;
        int off=qmi_sdu_parse_header(b,(size_t)r,SERVICE,&h);
        if(off<0) die("malformed QMI header");
        if(!valid_tlvs(b+off,h.tlv_len)) die("malformed QMI TLVs");
        if(h.ctl_flag==QMI_INDICATION) {
            if(h.msg_id==0x2b) msa=1;
            if(h.msg_id==0x21) ready=1;
            if(!txn) return 1;
            continue;
        }
        if(h.ctl_flag!=QMI_RESPONSE || h.txn!=txn || h.msg_id!=msg) continue;
        const uint8_t *v; size_t len;
        if(tlv_find(b+off,h.tlv_len,2,&v,&len) || len!=4) die("missing QMI result");
        result_code=le16(v); error_code=le16(v+2);
        memcpy(reply,b+off,h.tlv_len); reply_len=h.tlv_len;
        return 1;
    }
}
static void request_result(uint16_t msg,const uint8_t *b,size_t len) {
    uint8_t packet[8192]; if(++transaction==0) ++transaction;
    ssize_t n=qmi_sdu_build(packet,sizeof(packet),SERVICE,QMI_REQUEST,transaction,msg,b,len);
    if(n<0 || qrtr_sendto(fd,node,port,packet,(unsigned)n)<0) die("send failed");
    if(!receive(now()+10000,transaction,msg)) die("QMI response timeout");
}
static void check_result(uint16_t msg) {
    if(result_code || error_code) {
        fprintf(stderr,"wlan-fw: QMI 0x%04x result %u error %u\n",msg,result_code,error_code);
        exit(1);
    }
}
static void request(uint16_t msg,const uint8_t *b,size_t len) {
    request_result(msg,b,len); check_result(msg);
}
/* Hash the same open file used for transfer, through the rootfs checksum tool.
 * No shell, firmware-path interpolation, or alternate generic BDF is involved.
 */
static int chef_hash_matches(FILE *f) {
    const char expected[]="b72b699075a087fe5c299a83768f2c2c1a8dbbea576099c80183970f32b8c43c";
    int channel[2]; if(pipe(channel)) die("checksum pipe failed");
    pid_t child=fork(); if(child<0) die("checksum fork failed");
    if(!child) {
        if(dup2(fileno(f),STDIN_FILENO)<0 || dup2(channel[1],STDOUT_FILENO)<0) _exit(127);
        close(channel[0]); close(channel[1]);
        execlp("sha256sum","sha256sum",(char *)NULL); _exit(127);
    }
    close(channel[1]);
    char digest[65]; size_t used=0;
    while(used<sizeof(digest)) {
        ssize_t n=read(channel[0],digest+used,sizeof(digest)-used);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        used+=(size_t)n;
    }
    close(channel[0]); int status;
    while(waitpid(child,&status,0)<0) if(errno!=EINTR) die("checksum wait failed");
    rewind(f);
    return WIFEXITED(status) && WEXITSTATUS(status)==0 && used==65 &&
        digest[64]==' ' && !memcmp(digest,expected,64);
}
static int chef_boot_identity(void) {
    FILE *model=fopen("/sys/firmware/devicetree/base/model","rb");
    if(!model) model=fopen("/proc/device-tree/model","rb");
    if(!model) return 0;
    char value[6]; size_t len=fread(value,1,sizeof(value),model);
    int matched=!ferror(model) && len==5 && !memcmp(value,"chef\0",5);
    fclose(model); return matched;
}
static int chef_cap_matches(void) {
    const uint8_t *v; size_t len;
    if(tlv_find(reply,reply_len,0x10,&v,&len) || len!=8 || le32(v)!=0x140 || le32(v+4)!=0x4002) return 0;
    if(tlv_find(reply,reply_len,0x12,&v,&len) || len!=4 || le32(v)!=0x40050000) return 0;
    if(tlv_find(reply,reply_len,0x13,&v,&len) || len<4 || len>37 || le32(v)!=0x101402cd) return 0;
    return 1;
}
static void wait_flag(int *flag) {
    int64_t deadline=now()+60000;
    while(!*flag) if(!receive(deadline,0,0)) die("firmware indication timeout");
}
static void download(const char *directory,uint32_t board,int chef_cap) {
    char path[512];
    if(board==0xff) snprintf(path,sizeof(path),"%s/bdwlan_chef.bin",directory);
    else if(board<=0xff) snprintf(path,sizeof(path),"%s/bdwlan.b%02x",directory,board);
    else snprintf(path,sizeof(path),"%s/bdwlan.%03x",directory,board);
    FILE *f=fopen(path,"rb"); if(!f) die("board-specific BDF absent; refusing generic fallback");
    if(fseek(f,0,SEEK_END)) die("BDF seek failed");
    long size=ftell(f); if(size<=0 || size>LIMIT) die("invalid BDF size"); rewind(f);
    int chef_compat=board==0xff && chef_cap && chef_boot_identity() && chef_hash_matches(f);
    fprintf(stderr,"wlan-fw: board 0x%x, BDF %ld bytes\n",board,size);
    uint32_t offset=0,segment=0;
    while(offset<(uint32_t)size) {
        uint8_t b[8192],data[CHUNK+2],one=1,end,type=0; size_t n=0;
        size_t count=(uint32_t)size-offset; if(count>CHUNK) count=CHUNK;
        data[0]=(uint8_t)count; data[1]=(uint8_t)(count>>8);
        if(fread(data+2,1,count,f)!=count) die("BDF read failed");
        end=offset+count==(uint32_t)size;
        add(b,&n,1,&one,1); add32(b,&n,0x10,0);
        add32(b,&n,0x11,(uint32_t)size); add32(b,&n,0x12,segment);
        add(b,&n,0x13,data,count+2); add(b,&n,0x14,&end,1); add(b,&n,0x15,&type,1);
        request_result(0x25,b,n);
        if(result_code || error_code) {
            if(!(chef_compat && segment>0 && end && offset+count==(uint32_t)size &&
                 result_code==1 && error_code==1)) check_result(0x25);
            /* Exact stock chef cnss-daemon was observed continuing here after
             * complete transfer. All earlier chunks must already have passed.
             * Still require CAL_REPORT success and an actual FW_READY event.
             */
            fputs("wlan-fw: WARNING: exact stock chef final BDF result=1 error=1; following verified vendor CAL_REPORT lifecycle\n",stderr);
        }
        offset+=(uint32_t)count; segment++;
    }
    fclose(f);
}
int main(int argc,char **argv) {
    if(argc!=2) die("usage: wlan-fw BDF-directory (gps-up must already be running)");
    fd=qrtr_open(0); if(fd<0) die("AF_MSM_IPC unavailable; start gps-up first");
    struct msm_ipc_server_info peers[16];
    int count=msmipc_lookup(fd,SERVICE,0,peers,16),found=0;
    for(int i=0;i<count;i++) if(peers[i].instance==1) {
        if(found++) die("ambiguous WLFW service");
        node=peers[i].node_id; port=peers[i].port_id;
    }
    if(!found) die("WLFW v1/instance0 unavailable; start gps-up and wait for wlan_pd UP");
    uint8_t b[8192],one=1; size_t n=0;
    add(b,&n,0x10,&one,1); add(b,&n,0x13,&one,1);
    /* Stock userspace DMON identity; never impersonate kernel KNEL. */
    add32(b,&n,0x15,0x444d4f4e);
    request(0x20,b,n);
    const uint8_t *v; size_t len;
    int r=tlv_find(reply,reply_len,0x10,&v,&len);
    if(r==-2 || (!r && len!=8)) die("invalid firmware status");
    if(!r) { ready |= !!(v[0]&2); msa |= !!(v[0]&4); }
    if(ready) { puts("wlan-fw: firmware already ready"); close(fd); return 0; }
    wait_flag(&msa);
    request(0x24,NULL,0);
    r=tlv_find(reply,reply_len,0x11,&v,&len);
    if(r==-2 || (!r && len!=4)) die("malformed CAP board_info");
    /* Match icnss: optional absent board_info means unknown board 0xff. */
    uint32_t board=r==-1 ? 0xff : le32(v);
    int chef_cap=chef_cap_matches();
    download(argv[1],board,chef_cap);
    uint8_t zero=0; n=0; add(b,&n,1,&zero,1); request(0x26,b,n);
    wait_flag(&ready);
    puts("wlan-fw: firmware ready"); close(fd); return 0;
}
