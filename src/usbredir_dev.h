#pragma once

/*
 * usbredir_dev — presents the emulated BE-300 USB function controller
 * (src/hw/usbf.c) to a USB host over the usbredir protocol
 * (spice usbredir, docs/usb-redirection-protocol.md upstream).
 *
 * The emulator plays the usbredir "usb-host" role (the side that owns the
 * device).  QEMU's stock `-device usb-redir,chardev=...` is the
 * "usb-guest" role and attaches the device to the VM's USB controller, so a
 * UTM Windows VM enumerates the guest-defined 07CF:2002 device with no
 * QEMU changes.  See docs/PC_CONNECT_BRIDGE.md for the UTM setup.
 */

#include <stdbool.h>

#include "host_chardev.h"
#include "hw/usbf.h"

typedef struct {
    host_chardev_kind_t kind;
    const char *target;
    const char *tee_path;   /* optional decoded packet log */
    bool trace;
} usbredir_dev_config_t;

/* Open the host stream.  Returns false (and prints why) on error. */
bool usbredir_dev_configure(const usbredir_dev_config_t *cfg);
/* Connect the controller model; installs its host-side callbacks. */
void usbredir_dev_bind(usbf_state_t *u);
bool usbredir_dev_active(void);

/* Cable state: VBUS from the PC.  The device is offered to the host only
 * while the cable is present and the guest has brought the controller up
 * with valid descriptors. */
void usbredir_dev_set_present(bool present);

/* Accept/redial, parse host packets, move endpoint data. */
void usbredir_dev_tick(void);
void usbredir_dev_shutdown(void);
