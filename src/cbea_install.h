/*
 * Offline installer for Casio CASSIOPEIA application packages (.cbea).
 *
 * Applies a package's Install.inf directly to a BE-300 NAND image: the
 * [Copy Files] payloads land in the \Nand Disk FAT16 volume and the
 * [Add Registry] entries are merged into the compact \Backup\System.reg
 * snapshot. This stands in for the PC-side Setup.exe -> PC Connect
 * appmgr.exe -> device CoInstall.exe path, so apps can be installed
 * without a Windows host. See docs/INSTALLING_APPS.md.
 *
 * Pure C with no emulator dependencies; used by the be300-install CLI,
 * the web frontend and tests/test_cbea_install.c.
 */

#ifndef CBEA_INSTALL_H
#define CBEA_INSTALL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Install one .cbea package into a NAND image (raw restore-image layout,
 * or a bare FAT16 volume starting at sector 0).
 *
 * The image is modified only when the whole install succeeds. Progress and
 * error text is appended to `log` (always NUL-terminated when log_len > 0).
 * Returns 0 on success, -1 on failure.
 */
int cbea_install(uint8_t *image, size_t image_len,
                 const uint8_t *pkg, size_t pkg_len,
                 char *log, size_t log_len);

/*
 * Read a file from the image's FAT16 volume. `path` uses backslashes and
 * is relative to the volume root (e.g. "Backup\\System.reg"). On success
 * returns a malloc'd buffer the caller frees and stores its size in
 * *out_len; returns NULL if the volume or file is missing.
 */
uint8_t *cbea_fat_read_file(const uint8_t *image, size_t image_len,
                            const char *path, size_t *out_len);

/*
 * Write `data` to `path` (backslashes, relative to the volume root) in the
 * image's FAT16 volume, creating intermediate directories and replacing an
 * existing file. Error text goes to `log`. Returns 0 on success, -1 on error.
 */
int cbea_fat_write_file(uint8_t *image, size_t image_len, const char *path,
                        const uint8_t *data, size_t len,
                        char *log, size_t log_len);

/*
 * Format the whole buffer as an empty bare FAT16 volume (no MBR), the same
 * layout tools/make_cf_image.sh produces for CF images. `image_len` must be
 * a multiple of 512 between 8 MB and 1 GB. Returns 0 on success, -1 on error.
 */
int cbea_fat_format(uint8_t *image, size_t image_len, const char *label);

#endif /* CBEA_INSTALL_H */
