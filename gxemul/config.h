/*
 *  config.h for be300-framebuffer GXemul integration.
 *  MIPS-only build, no X11, no networking.
 */

#ifndef CONFIG_H
#define CONFIG_H

#define VERSION "0.7.0-be300"
#define COMPILE_DATE "be300-framebuffer GXemul integration"

/*  MIPS only — no other CPU families  */
#define ADD_ALL_CPU_FAMILIES \
	add_cpu_family(mips_cpu_family_init, ARCH_MIPS);

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define HOST_LITTLE_ENDIAN

#undef mips

#endif  /*  CONFIG_H  */
