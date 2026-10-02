/* Simulated peer exercises the actual helper request/response state machine. */
#include <assert.h>
#include <poll.h>
#include <stdio.h>
static FILE *fake_fopen(const char *,const char *);
static int fake_poll(struct pollfd *, nfds_t, int);
#define poll fake_poll
#define fopen fake_fopen
#define execlp fake_execlp
#define main wlan_fw_main
#include "../wlan-fw.c"
#undef fopen
#undef execlp
#undef main
#undef poll
static uint16_t pending_msg, pending_txn;
static int phase, chunks, omit_board;
static int chef_boot_ok=1;
static int compat_case, hash_good, bad_identity, error_segment=-1, response_error=1, cal_error, omit_ready;
static size_t transferred;
static int fake_poll(struct pollfd *p,nfds_t n,int timeout) {
    assert(n==1 && timeout>0);
    if(omit_ready && pending_msg==0x26 && phase>=3) return 0;
    p->revents=POLLIN; return 1;
}
static FILE *fake_fopen(const char *path,const char *mode) {
    if(!strcmp(path,"/sys/firmware/devicetree/base/model") || !strcmp(path,"/proc/device-tree/model")) {
        if(chef_boot_ok==2) return NULL;
        FILE *f=tmpfile(); assert(f);
        if(chef_boot_ok) assert(fwrite("chef\0",1,5,f)==5);
        else assert(fwrite("other\0",1,6,f)==6);
        rewind(f); return f;
    }
    return fopen(path,mode);
}
int fake_execlp(const char *name,const char *arg,...) {
    assert(!strcmp(name,"sha256sum") && !strcmp(arg,"sha256sum"));
    const char *digest=hash_good ? "b72b699075a087fe5c299a83768f2c2c1a8dbbea576099c80183970f32b8c43c  -\n" : "0000000000000000000000000000000000000000000000000000000000000000  -\n";
    assert(write(STDOUT_FILENO,digest,strlen(digest))==(ssize_t)strlen(digest));
    _exit(0);
}
int qrtr_open(int rport) { assert(rport==0); return dup(1); }
int msmipc_lookup(int sock,uint32_t service,uint32_t instance,struct msm_ipc_server_info *out,int max) {
    (void)sock; assert(service==0x45 && instance==0 && max>=2);
    out[0]=(struct msm_ipc_server_info){9,9,0x45,2};
    out[1]=(struct msm_ipc_server_info){3,7,0x45,1}; return 2;
}
int qrtr_sendto(int sock,uint32_t n,uint32_t p,const void *data,unsigned size) {
    (void)sock; assert(n==3 && p==7);
    struct qmi_sdu_header h; const uint8_t *b=data,*v; size_t len;
    assert(qmi_sdu_parse_header(b,size,0x45,&h)==7);
    assert(h.ctl_flag==QMI_REQUEST); pending_msg=h.msg_id; pending_txn=h.txn; phase=0;
    b+=7;
    if(h.msg_id==0x20) {
        assert(!tlv_find(b,h.tlv_len,0x13,&v,&len) && len==1 && *v==1);
        assert(!tlv_find(b,h.tlv_len,0x15,&v,&len) && len==4 && le32(v)==0x444d4f4e);
        assert(le32(v)!=0x4b4e454c); /* separate from kernel KNEL */
    } else if(h.msg_id==0x25) {
        assert(!tlv_find(b,h.tlv_len,0x12,&v,&len) && len==4 && le32(v)==(uint32_t)chunks);
        assert(!tlv_find(b,h.tlv_len,0x13,&v,&len) && len>=2 && len<=CHUNK+2);
        assert(le16(v)==len-2);
        for(size_t i=2;i<len;i++) assert(v[i]==(uint8_t)(transferred+i-2));
        transferred+=len-2;
        assert(!tlv_find(b,h.tlv_len,0x14,&v,&len) && len==1 && *v==(transferred==12289));
        chunks++;
    } else if(h.msg_id==0x26) {
        assert(transferred==12289 && chunks==3);
        assert(!tlv_find(b,h.tlv_len,1,&v,&len) && len==1 && *v==0);
    } else assert(h.msg_id==0x24);
    return 0;
}
int qrtr_recvfrom(int sock,void *buf,unsigned cap,uint32_t *n,uint32_t *p) {
    (void)sock;
    if(phase++==0) return QRTR_RECV_RESUME_TX; /* addresses deliberately untouched */
    *n=3; *p=7;
    if(pending_msg==0x20 && phase==2)
        return (int)qmi_sdu_build(buf,cap,0x45,QMI_INDICATION,0,0x2b,NULL,0);
    if(pending_msg==0x26 && phase==2 && !omit_ready)
        return (int)qmi_sdu_build(buf,cap,0x45,QMI_INDICATION,0,0x21,NULL,0);
    uint8_t b[100],v[8]={0}; size_t len=0;
    int failed=(pending_msg==0x25 && chunks-1==error_segment) || (pending_msg==0x26 && cal_error);
    len=(size_t)tlv_put_result(b,sizeof(b),failed ? 1 : 0,failed ? response_error : 0);
    if(pending_msg==0x20) add(b,&len,0x10,v,8); /* stale status must not erase MSA */
    if(pending_msg==0x24 && !omit_board) { put32(v,0xff); add(b,&len,0x11,v,4); }
    if(pending_msg==0x24 && compat_case) {
        put32(v,bad_identity==1 ? 0x141 : 0x140); put32(v+4,bad_identity==2 ? 0x4003 : 0x4002); add(b,&len,0x10,v,8);
        put32(v,bad_identity==3 ? 0x40050001 : 0x40050000); add(b,&len,0x12,v,4);
        put32(v,bad_identity==4 ? 0x101402ce : 0x101402cd); add(b,&len,0x13,v,4);
    }
    return (int)qmi_sdu_build(buf,cap,0x45,QMI_RESPONSE,pending_txn,pending_msg,b,len);
}
int main(void) {
    uint8_t bad[]={0x11,4,0,0xff,0,0,0,0x99};
    assert(!valid_tlvs(bad,sizeof(bad)));
    assert(valid_tlvs(bad,sizeof(bad)-1));
    char directory[]="/tmp/wlan-fw-test.XXXXXX"; assert(mkdtemp(directory));
    char path[256]; snprintf(path,sizeof(path),"%s/bdwlan_chef.bin",directory);
    FILE *f=fopen(path,"wb"); assert(f);
    for(int i=0;i<12289;i++) fputc(i&255,f);
    fclose(f);
    char *args[]={"wlan-fw",directory,NULL}; assert(wlan_fw_main(2,args)==0);
    assert(msa && ready && chunks==3);
    msa=ready=chunks=0; transferred=0; omit_board=1;
    assert(wlan_fw_main(2,args)==0);
    assert(msa && ready && chunks==3);
    /* Each failure case must exit before success, in an isolated process. */
    fflush(NULL);
    for(int scenario=0;scenario<14;scenario++) {
        pid_t child=fork(); assert(child>=0);
        if(!child) {
            msa=ready=chunks=0; transferred=0; compat_case=1; hash_good=1;
            error_segment=2; response_error=1; cal_error=omit_ready=bad_identity=0;
            if(scenario==1) hash_good=0;
            if(scenario>=2 && scenario<=5) bad_identity=scenario-1;
            if(scenario==6) error_segment=0;
            if(scenario==7) error_segment=1;
            if(scenario==8) response_error=2;
            if(scenario==9) cal_error=1;
            if(scenario==10) omit_ready=1;
            if(scenario==11) compat_case=0;
            if(scenario==12) chef_boot_ok=0;
            if(scenario==13) chef_boot_ok=2;
            int result=wlan_fw_main(2,args);
            _exit(result);
        }
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status));
        assert(WEXITSTATUS(status)==(scenario==0 ? 0 : 1));
    }
    unlink(path); rmdir(directory);
    puts("wlan-fw flow control, stale status, peer selection and BDF chunks: PASS");
    return 0;
}
