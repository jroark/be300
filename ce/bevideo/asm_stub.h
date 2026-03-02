#ifndef _BEVIDEO_ASM_STUB_H
#define _BEVIDEO_ASM_STUB_H

#define NESTED(symbol, framesize, rpc) \
    .globl symbol; \
    .align 2; \
    .type symbol, @function; \
    symbol: ; \
    .ent symbol

#define END(symbol) \
    .end symbol; .size symbol, .-symbol

#endif
