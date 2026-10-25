#include "wifi_security.h"
#include <stdlib.h>
#include <string.h>
#ifdef BE300_WIFI_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif
#ifdef BE300_WIFI_OPENSSL
static unsigned be16(const uint8_t *p) { return p[0]*256u+p[1]; }
static void put16(uint8_t *p, unsigned v) { p[0]=v>>8; p[1]=v; }
#endif
static int hex(unsigned c)
{
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
void wifi_security_reset(wifi_security_t *s)
{
    s->stage=s->pending_len=0; s->authorized=s->pairwise=s->group=false;
    memset(s->ptk,0,sizeof(s->ptk));
}
bool wifi_security_configure(wifi_security_t *s, unsigned mode, const char *key)
{
    wifi_security_reset(s); s->valid=false; s->mode=mode; s->ie_len=0;
    memset(s->secret,0,sizeof(s->secret)); memset(s->pmk,0,sizeof(s->pmk));
    size_t n=key?strlen(key):0;
    if (mode>3 || n>64) return false;
    if (mode==1) {
        if (n!=5 && n!=13 && n!=10 && n!=26) return false;
        bool h=n==10 || n==26; s->secret_len=h?n/2:n;
        for (unsigned i=0;i<s->secret_len;i++) {
            if (h && (hex(key[2*i])<0 || hex(key[2*i+1])<0)) return false;
            s->secret[i]=h?hex(key[2*i])*16+hex(key[2*i+1]):key[i];
        }
    } else if (mode>=2) {
#ifdef BE300_WIFI_OPENSSL
        if (n<8) return false;
        if (n==64) {
            for (unsigned i=0;i<32;i++) {
                if (hex(key[2*i])<0 || hex(key[2*i+1])<0) return false;
                s->pmk[i]=hex(key[2*i])*16+hex(key[2*i+1]);
            }
        } else if (!PKCS5_PBKDF2_HMAC_SHA1(key,n,(const unsigned char *)"BE300",5,4096,32,s->pmk)) return false;
        static const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
        static const uint8_t wpa[]={221,22,0,80,242,1,1,0,0,80,242,2,1,0,0,80,242,2,1,0,0,80,242,2};
        s->ie_len=mode==3?sizeof(rsn):sizeof(wpa);
        memcpy(s->ie,mode==3?rsn:wpa,s->ie_len);
#else
        return false;
#endif
    }
    s->valid=true; return true;
}
void wifi_security_init(wifi_security_t *s, const uint8_t mac[6])
{
    memset(s,0,sizeof(*s)); memcpy(s->mac,mac,6);
    static const uint8_t bssid[]={2,0x42,0x45,0x33,0x30,0x30}; memcpy(s->bssid,bssid,6);
    const char *mode=getenv("BE300_WIFI_SECURITY"), *key=getenv("BE300_WIFI_KEY");
    unsigned value=!mode || !strcmp(mode,"open")?0:!strcmp(mode,"wep")?1:!strcmp(mode,"wpa")?2:!strcmp(mode,"wpa2")?3:99;
    wifi_security_configure(s,value,key);
}
bool wifi_security_wep(const wifi_security_t *s, unsigned index, const uint8_t *key, unsigned n)
{
    /* The fixture accepts the selected transmit slot; all four are exercised. */
    return s->valid && s->mode==1 && index<4 && n==s->secret_len && !memcmp(key,s->secret,n);
}
#ifdef BE300_WIFI_OPENSSL
static bool hmac(unsigned mode,const uint8_t *key,unsigned nk,const uint8_t *data,unsigned n,uint8_t *out)
{
    unsigned l=0; return HMAC(mode==2?EVP_sha1():EVP_md5(),key,nk,data,n,out,&l)!=NULL;
}
static void rc4(uint8_t *data,unsigned n,const uint8_t *key,unsigned nk)
{
    uint8_t table[256]; unsigned j=0;
    for(unsigned i=0;i<256;i++)table[i]=i;
    for(unsigned i=0;i<256;i++){j=(j+table[i]+key[i%nk])&255;uint8_t t=table[i];table[i]=table[j];table[j]=t;}
    unsigned i=0;j=0;
    for(unsigned k=0;k<n+256;k++){i=(i+1)&255;j=(j+table[i])&255;uint8_t t=table[i];table[i]=table[j];table[j]=t;
        if(k>=256)data[k-256]^=table[(table[i]+table[j])&255];}
}
static bool mic(wifi_security_t *s,uint8_t *e,unsigned n)
{
    uint8_t hash[EVP_MAX_MD_SIZE];memset(e+81,0,16);
    if(!hmac(s->mode==3?2:1,s->ptk,16,e,n,hash))return false;
    memcpy(e+81,hash,16);return true;
}
static bool build(wifi_security_t *s,unsigned info,const uint8_t *key,unsigned nk)
{
    uint8_t *p=s->pending,*e=p+14;
    memset(p,0,sizeof(s->pending));memcpy(p,s->mac,6);memcpy(p+6,s->bssid,6);p[12]=0x88;p[13]=0x8e;
    e[0]=2;e[1]=3;e[4]=s->mode==3?2:254;put16(e+5,info);put16(e+7,s->mode==3?16:32);
    for(unsigned i=0;i<8;i++)e[9+i]=(uint8_t)(s->replay>>(56-8*i));
    if(info&8)memcpy(e+17,s->anonce,32);
    if(nk)memcpy(e+99,key,nk);put16(e+97,nk);put16(e+2,95+nk);
    if((info&256)&&!mic(s,e,99+nk))return false;
    s->pending_len=14+99+nk;return true;
}
static bool derive(wifi_security_t *s,const uint8_t *snonce)
{
    uint8_t data[100],input[128],hash[EVP_MAX_MD_SIZE];
    const uint8_t *a=s->mac,*b=s->bssid;
    if(memcmp(a,b,6)>0){const uint8_t *t=a;a=b;b=t;}
    memcpy(data,a,6);memcpy(data+6,b,6);
    a=s->anonce;b=snonce;if(memcmp(a,b,32)>0){const uint8_t *t=a;a=b;b=t;}
    memcpy(data+12,a,32);memcpy(data+44,b,32);
    static const char label[]="Pairwise key expansion";
    memcpy(input,label,sizeof(label));memcpy(input+sizeof(label),data,76);
    unsigned off=0;
    for(unsigned i=0;off<64;i++) {
        input[sizeof(label)+76]=i;
        if(!hmac(2,s->pmk,32,input,sizeof(label)+77,hash))return false;
        unsigned n=64-off<20?64-off:20;memcpy(s->ptk+off,hash,n);off+=n;
    }
    return true;
}
static bool message3(wifi_security_t *s)
{
    uint8_t data[128];unsigned n=s->ie_len;memcpy(data,s->ie,n);
    s->replay++;
    if(s->mode==2)return build(s,0x01c9,data,n);
    data[n++]=221;data[n++]=22;memcpy(data+n,"\x00\x0f\xac\x01\x01\x00",6);n+=6;
    memcpy(data+n,s->gtk,16);n+=16;
    if(n%8){data[n++]=221;while(n%8)data[n++]=0;}
    uint8_t wrapped[160];int nw=0,last=0;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;
    EVP_CIPHER_CTX_set_flags(ctx,EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    bool ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_wrap(),NULL,s->ptk+16,NULL)&&
        EVP_EncryptUpdate(ctx,wrapped,&nw,data,n)&&EVP_EncryptFinal_ex(ctx,wrapped+nw,&last);
    EVP_CIPHER_CTX_free(ctx);
    return ok && build(s,0x13ca,wrapped,nw+last);
}
static bool group1(wifi_security_t *s)
{
    uint8_t data[32],key[32],iv[16];
    if(!RAND_bytes(iv,16))return false;
    memcpy(key,iv,16);memcpy(key+16,s->ptk+16,16);memcpy(data,s->gtk,32);rc4(data,32,key,32);
    s->replay++;
    if(!build(s,0x0391,data,32))return false;
    memcpy(s->pending+14+49,iv,16);return mic(s,s->pending+14,131);
}
#endif
void wifi_security_start(wifi_security_t *s)
{
    wifi_security_reset(s);
    if(!s->valid)return;
    if(s->mode<2){s->authorized=true;return;}
#ifdef BE300_WIFI_OPENSSL
    if(!RAND_bytes(s->anonce,32)||!RAND_bytes(s->gtk,32))return;
    s->replay=1;
    if(build(s,s->mode==3?0x008a:0x0089,NULL,0))s->stage=1;
#endif
}
bool wifi_security_tx(wifi_security_t *s,const uint8_t *p,size_t n)
{
    if(n<14 || !s->valid)return false;
    if(p[12]!=0x88 || p[13]!=0x8e)return s->authorized && (s->mode<2 || (s->pairwise&&s->group));
#ifdef BE300_WIFI_OPENSSL
    if(s->mode<2 || n<113 || memcmp(p,s->bssid,6)||memcmp(p+6,s->mac,6))return false;
    const uint8_t *e=p+14;unsigned length=be16(e+2)+4,info=be16(e+5);
    if(length<99 || length>n-14 || length>400 || e[1]!=3 || !(info&256) ||
        (info&128) || (info&7)!=(s->mode==3?2:1) || be16(e+97)!=length-99)return false;
    uint64_t replay=0;for(unsigned i=0;i<8;i++)replay=(replay<<8)|e[9+i];
    if(replay!=s->replay)return false;
    if(s->stage==1 && !derive(s,e+17))return false;
    uint8_t copy[400],original[16];memcpy(copy,e,length);memcpy(original,e+81,16);
    if(!mic(s,copy,length)||CRYPTO_memcmp(copy+81,original,16))return false;
    if(s->stage==1 && (info&8)) {
        if(message3(s))s->stage=2;
    } else if(s->stage==2 && (info&8) && be16(e+97)==0) {
        if(s->mode==3){s->stage=4;s->authorized=true;}
        else if(group1(s))s->stage=3;
    } else if(s->stage==3 && !(info&8)) {s->stage=4;s->authorized=true;}
#else
    (void)s;
#endif
    return false; /* EAPOL belongs to this virtual AP, never the NAT backend. */
}
unsigned wifi_security_pop(wifi_security_t *s,uint8_t *p,unsigned n)
{
    if(!s->pending_len || n<s->pending_len)return 0;
    unsigned count=s->pending_len;memcpy(p,s->pending,count);s->pending_len=0;return count;
}
bool wifi_security_key(wifi_security_t *s,bool pairwise,unsigned cipher,const uint8_t *key,unsigned n)
{
    if(!n){if(pairwise)s->pairwise=false;else s->group=false;return true;}
    if(s->stage<2)return false;
    unsigned expected=s->mode==3?16:32;
    if(n!=expected || cipher!=(s->mode==3?2u:1u))return false;
    uint8_t target[32];memcpy(target,pairwise?s->ptk+32:s->gtk,n);
    /* Supplicant presents TKIP group Michael keys in station TX/RX order. */
    if(cipher==1){const uint8_t *base=pairwise?s->ptk+32:s->gtk;memcpy(target+16,base+24,8);memcpy(target+24,base+16,8);}
    if(memcmp(target,key,n))return false;
    if(pairwise)s->pairwise=true;else s->group=true;return true;
}

bool wifi_security_rekey(wifi_security_t *s)
{
#ifdef BE300_WIFI_OPENSSL
    if(s->stage!=4 || !RAND_bytes(s->gtk,32))return false;
    s->group=false;s->stage=3;
    if(s->mode==2)return group1(s);
    uint8_t data[24]={221,22,0,15,172,1,1,0},wrapped[40];
    memcpy(data+8,s->gtk,16);int n=0,last=0;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;
    EVP_CIPHER_CTX_set_flags(ctx,EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    bool ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_wrap(),NULL,s->ptk+16,NULL)&&
        EVP_EncryptUpdate(ctx,wrapped,&n,data,sizeof(data))&&EVP_EncryptFinal_ex(ctx,wrapped+n,&last);
    EVP_CIPHER_CTX_free(ctx);s->replay++;
    return ok && build(s,0x1382,wrapped,n+last);
#else
    (void)s;return false;
#endif
}
