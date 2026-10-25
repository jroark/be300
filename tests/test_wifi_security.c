#include "hw/wifi_security.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    const uint8_t mac[]={2,3,4,5,6,7};wifi_security_t ap;
    wifi_security_init(&ap,mac);
    assert(!wifi_security_configure(&ap,99,"password"));
    uint8_t data[64]={0};assert(!wifi_security_tx(&ap,data,sizeof(data)));
    assert(wifi_security_configure(&ap,0,NULL));wifi_security_start(&ap);
    assert(wifi_security_tx(&ap,data,sizeof(data)));
    assert(wifi_security_configure(&ap,1,"0102030405"));
    const uint8_t key[]={1,2,3,4,5};
    for(unsigned i=0;i<4;i++)assert(wifi_security_wep(&ap,i,key,sizeof(key)));
    assert(!wifi_security_wep(&ap,4,key,sizeof(key)));
    assert(!wifi_security_configure(&ap,1,"bad"));
    for(unsigned mode=2;mode<=3;mode++) {
        assert(wifi_security_configure(&ap,mode,"password"));wifi_security_start(&ap);
        assert(!ap.authorized && !wifi_security_tx(&ap,data,sizeof(data)));
        uint8_t first[512],next[512],zero[32]={0};
        unsigned n=wifi_security_pop(&ap,first,sizeof(first));assert(n==113);
        assert(first[12]==0x88 && first[13]==0x8e && first[15]==3);
        assert(!wifi_security_key(&ap,true,mode==3?2:1,zero,mode==3?16:32));
        assert(!wifi_security_tx(&ap,first,12));
        wifi_security_start(&ap);assert(wifi_security_pop(&ap,next,sizeof(next))==n);
        assert(memcmp(first+31,next+31,32));
        wifi_security_reset(&ap);assert(!ap.authorized && !ap.pending_len);
        assert(!memcmp(ap.ptk,zero,32));
    }
    puts("wifi security fixture: configuration, WEP, nonce freshness, EAPOL and traffic gating PASS");
}
