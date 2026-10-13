#pragma once

/*
 * host_chardev — non-blocking host byte stream used by the emulator's
 * external links (PC Connect serial bridge, usbredir USB device).
 *
 * Supports tcp:HOST:PORT, tcp-listen:PORT[@ADDR], unix:/PATH,
 * unix-listen:/PATH and pty:auto.  Connect modes redial every 500 ms while
 * no peer is attached; listen modes accept one peer at a time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include "win32_compat.h"
#else
#include <sys/types.h>
#endif

#ifdef _WIN32
typedef intptr_t host_chardev_handle_t;
#define HOST_CHARDEV_INVALID ((host_chardev_handle_t)-1)
#else
typedef int host_chardev_handle_t;
#define HOST_CHARDEV_INVALID (-1)
#endif

typedef enum {
    HOST_CHARDEV_NONE = 0,
    HOST_CHARDEV_TCP_CONNECT,    /* tcp:host:port           */
    HOST_CHARDEV_TCP_LISTEN,     /* tcp-listen:port[@addr]  */
    HOST_CHARDEV_UNIX_CONNECT,   /* unix:/path              */
    HOST_CHARDEV_UNIX_LISTEN,    /* unix-listen:/path       */
    HOST_CHARDEV_PTY,            /* pty:auto                */
} host_chardev_kind_t;

typedef struct host_chardev {
    host_chardev_kind_t kind;
    const char *tag;        /* log prefix; borrowed, must outlive the chardev */
    bool trace;

    /* listen_fd is valid only in *_LISTEN modes; fd only while a peer is
     * attached (or, in connect modes, while connect() is in flight). */
    host_chardev_handle_t listen_fd;
    host_chardev_handle_t fd;

    /* PTY mode only: an internal slave fd held open for the pty's lifetime.
     * See host_chardev.c:open_pty. */
    host_chardev_handle_t pty_slave_fd;

    char *connect_host;
    int connect_port;
    char *unix_path;

    bool connecting;          /* fd holds a non-blocking connect() */
    bool new_peer;            /* a peer became usable; see take_new_peer */
    uint64_t next_reconnect_ns;
} host_chardev_t;

/* Split "<kind>:<target>" into kind and target (target borrows from spec).
 * Prints to stderr and returns false if malformed. */
bool host_chardev_parse_spec(const char *spec, host_chardev_kind_t *kind,
    const char **target);

void host_chardev_init(host_chardev_t *c, const char *tag, bool trace);
bool host_chardev_open(host_chardev_t *c, host_chardev_kind_t kind,
    const char *target);
void host_chardev_close(host_chardev_t *c);

/* Accept a waiting peer or redial a lost connection. */
void host_chardev_tick(host_chardev_t *c);
/* True once fd is a usable connected stream.  Completes a pending
 * non-blocking connect (closing the fd on failure). */
bool host_chardev_ready(host_chardev_t *c);
/* Returns true once per newly accepted / connected peer. */
bool host_chardev_take_new_peer(host_chardev_t *c);
/* Close the current peer.  Listen and connect modes pick up the next peer
 * from host_chardev_tick(); a PTY needs host_chardev_reopen_pty(). */
void host_chardev_drop_peer(host_chardev_t *c);
bool host_chardev_reopen_pty(host_chardev_t *c);

ssize_t host_chardev_read(host_chardev_t *c, void *buf, size_t len);
ssize_t host_chardev_write(host_chardev_t *c, const void *buf, size_t len);
bool host_chardev_errno_would_block(void);
