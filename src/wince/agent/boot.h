/* boot.h - Linux/bare-metal loader for Windows CE 5 on the MT3351 (boot.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BF_BOOT_H
#define BF_BOOT_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    WCHAR kernel[MAX_PATH], dtb[MAX_PATH], initrd[MAX_PATH];
    DWORD kernel_pa, dtb_pa, initrd_pa;        /* destinations, all below 0x02000000 */
    DWORD kernel_max, dtb_max, initrd_max;
    DWORD machine;                             /* r1: 0xFFFFFFFF for device tree boots */
    int dry_run;                               /* prepare everything, do not jump */
} bf_boot_args;

/* Returns 0 with err filled on failure; on success it never returns (unless dry_run). */
int bf_boot(const bf_boot_args *args, char *err, size_t err_len);

#endif
