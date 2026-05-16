#ifndef BE300_WIN32_COMPAT_H
#define BE300_WIN32_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef far
#undef far
#endif
#ifdef near
#undef near
#endif

#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
typedef unsigned int uint;

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define random rand
#define srandom srand
#define isatty _isatty

int be300_win32_init(void);
void be300_win32_shutdown(void);

int be300_clock_gettime(int clock_id, struct timespec *ts);
int be300_gettimeofday(struct timeval *tv, void *tz);
int be300_nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int be300_sleep(unsigned int seconds);
int be300_usleep(unsigned int usec);

#define clock_gettime be300_clock_gettime
#define gettimeofday be300_gettimeofday
#define nanosleep be300_nanosleep
#define sleep be300_sleep
#define usleep be300_usleep

#endif /* _WIN32 */

#endif /* BE300_WIN32_COMPAT_H */
