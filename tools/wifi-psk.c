/* WPA2 PBKDF2-HMAC-SHA1 (4096 rounds). Passphrase is read only from stdin.
 * Outputs a network block with hex SSID and derived key, never plaintext.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct sha1 { uint32_t h[5]; uint64_t bytes; unsigned used; uint8_t block[64]; };
static uint32_t rol(uint32_t x,unsigned n) { return (x<<n)|(x>>(32-n)); }
static void transform(struct sha1 *s,const uint8_t *b) {
    uint32_t w[80];
    for(unsigned i=0;i<16;i++) w[i]=(uint32_t)b[4*i]<<24|(uint32_t)b[4*i+1]<<16|(uint32_t)b[4*i+2]<<8|b[4*i+3];
    for(unsigned i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=s->h[0],c=s->h[2],d=s->h[3],e=s->h[4],bb=s->h[1];
    for(unsigned i=0;i<80;i++) {
        uint32_t f,k;
        if(i<20) { f=(bb&c)|(~bb&d); k=0x5a827999; }
        else if(i<40) { f=bb^c^d; k=0x6ed9eba1; }
        else if(i<60) { f=(bb&c)|(bb&d)|(c&d); k=0x8f1bbcdc; }
        else { f=bb^c^d; k=0xca62c1d6; }
        uint32_t t=rol(a,5)+f+e+k+w[i]; e=d; d=c; c=rol(bb,30); bb=a; a=t;
    }
    s->h[0]+=a; s->h[1]+=bb; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void init(struct sha1 *s) {
    memset(s,0,sizeof(*s));
    s->h[0]=0x67452301; s->h[1]=0xefcdab89; s->h[2]=0x98badcfe; s->h[3]=0x10325476; s->h[4]=0xc3d2e1f0;
}
static void update(struct sha1 *s,const void *data,size_t n) {
    const uint8_t *p=data; s->bytes+=n;
    while(n) {
        size_t take=64-s->used; if(take>n) take=n;
        memcpy(s->block+s->used,p,take); s->used+=(unsigned)take; p+=take; n-=take;
        if(s->used==64) { transform(s,s->block); s->used=0; }
    }
}
static void finish(struct sha1 *s,uint8_t out[20]) {
    uint64_t bits=s->bytes*8; uint8_t pad[64]={0x80},length[8];
    for(unsigned i=0;i<8;i++) length[7-i]=(uint8_t)(bits>>(i*8));
    unsigned count=s->used<56 ? 56-s->used : 120-s->used;
    update(s,pad,count); update(s,length,8);
    for(unsigned i=0;i<20;i++) out[i]=(uint8_t)(s->h[i/4]>>(24-(i%4)*8));
}
static void hmac(const uint8_t *key,size_t keylen,const uint8_t *data,size_t len,uint8_t out[20]) {
    uint8_t ipad[64],opad[64],digest[20]; struct sha1 s;
    for(unsigned i=0;i<64;i++) { uint8_t x=i<keylen ? key[i] : 0; ipad[i]=x^0x36; opad[i]=x^0x5c; }
    init(&s); update(&s,ipad,64); update(&s,data,len); finish(&s,digest);
    init(&s); update(&s,opad,64); update(&s,digest,20); finish(&s,out);
}
static void erase(void *data,size_t len) { volatile uint8_t *p=data; while(len--) *p++=0; }
int main(int argc,char **argv) {
    if(argc!=2 || strlen(argv[1])<1 || strlen(argv[1])>32) { fputs("wifi-psk: SSID must be 1..32 bytes\n",stderr); return 1; }
    uint8_t password[64],salt[36],psk[40],u[20],acc[20]; size_t n=0; int c;
    while((c=getchar())!=EOF && c!='\n') {
        if(n==sizeof(password)) { erase(password,sizeof(password)); fputs("wifi-psk: passphrase too long\n",stderr); return 1; }
        password[n++]=(uint8_t)c;
    }
    if(ferror(stdin) || n<8 || n>63) { erase(password,sizeof(password)); fputs("wifi-psk: passphrase must be 8..63 bytes\n",stderr); return 1; }
    size_t ssidlen=strlen(argv[1]); memcpy(salt,argv[1],ssidlen);
    for(unsigned block=1;block<=2;block++) {
        salt[ssidlen]=salt[ssidlen+1]=salt[ssidlen+2]=0; salt[ssidlen+3]=(uint8_t)block;
        hmac(password,n,salt,ssidlen+4,u); memcpy(acc,u,20);
        for(unsigned round=1;round<4096;round++) {
            hmac(password,n,u,20,u);
            for(unsigned i=0;i<20;i++) acc[i]^=u[i];
        }
        memcpy(psk+(block-1)*20,acc,20);
    }
    erase(password,sizeof(password));
    fputs("network={\n\tssid=",stdout);
    for(size_t i=0;i<ssidlen;i++) printf("%02x",(unsigned char)argv[1][i]);
    fputs("\n\tpsk=",stdout); for(unsigned i=0;i<32;i++) printf("%02x",psk[i]);
    fputs("\n}\n",stdout); erase(psk,sizeof(psk)); erase(u,sizeof(u)); erase(acc,sizeof(acc));
    return ferror(stdout) ? 1 : 0;
}
