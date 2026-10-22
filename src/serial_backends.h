#pragma once

/* Route the GXemul ns16550 UARTs to the BE-300 host backends (PC Connect,
 * Stowaway, plain serial bridges); see gxemul ns16550_backend.h. */
void be300_serial_backends_install(void);
