/*
 * BFP - BigFoston Probe protocol, version 1.
 *
 * Request/response link between the PC (tools/bigfoston/bfagent.py) and the
 * diagnostic agent running on the FOSTON under Windows CE (bfagent.c), over
 * the USB Serial_Class port (045e:00ce, Linux "ipaq" driver, /dev/ttyUSB*).
 *
 * Same framing as FSP (Big Screen Monitor) but a different magic, so the
 * Big Screen service and the agent never mistake each other's messages.
 *
 * Every message: 16-byte header + payload, little endian.
 *   0  'B'  1  'F'  2 version (1)  3 type
 *   4  u16 sequence (a response repeats the request's)
 *   6  u16 status (responses: 0 ok, otherwise BFP_E_*; requests: 0)
 *   8  u32 payload length (<= BFP_MAX_PAYLOAD)
 *  12  u16 CRC-16/CCITT-FALSE of the payload
 *  14  u16 CRC-16/CCITT-FALSE of bytes 0..13
 *
 * The host sends one request and waits for its response (stop and wait):
 * the Windows CE Serial_Class driver loses data when it is not drained,
 * so requests are kept <= BFP_MAX_REQUEST.
 *
 * Error responses carry: u32 Win32 error code, then a UTF-8 message.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BFP_H
#define BFP_H

#include <stddef.h>
#include <stdint.h>

#define BFP_MAGIC0 0x42
#define BFP_MAGIC1 0x46
#define BFP_VERSION 1
#define BFP_HEADER 16
#define BFP_MAX_PAYLOAD (1024u * 1024u)
#define BFP_MAX_REQUEST (32u * 1024u)
#define BFP_RESPONSE 0x80          /* response type = request type | BFP_RESPONSE */

/* requests (payload layout in bfagent.c and bfagent.py) */
#define BFP_INFO      0x01   /* -> text */
#define BFP_PING      0x02   /* bytes -> same bytes */
#define BFP_LIST      0x10   /* path -> lines "D|F <size> <attrs> <name>" */
#define BFP_READ      0x11   /* u32 offset, u32 length, path -> bytes */
#define BFP_WRITE     0x12   /* u32 offset, u8 truncate, path NUL, bytes -> u32 written */
#define BFP_MKDIR     0x13   /* path */
#define BFP_DELETE    0x14   /* path (files only) */
#define BFP_REG       0x20   /* u8 root, u8 depth, path -> text */
#define BFP_EXEC      0x30   /* path NUL args -> u32 pid */
#define BFP_PS        0x31   /* u8 with_modules -> text */
#define BFP_KILL      0x32   /* u32 pid */
#define BFP_PHYS      0x40   /* u32 address, u32 length, u8 access width (1 or 4) -> bytes */
#define BFP_CPU       0x41   /* -> text (CP15 registers, read in kernel mode) */
#define BFP_KIOCTL    0x42   /* u32 code (whitelisted), input -> output */
#define BFP_DISKINFO  0x50   /* device name ("DSK1:") -> u32 x6 DISK_INFO */
#define BFP_DISKREAD  0x51   /* u32 sector, u32 count, device name -> bytes */
#define BFP_SHOT      0x60   /* -> u16 width, u16 height, RGB565 rows top-down */
#define BFP_BOOT      0x38   /* u32 kernel_pa, u32 dtb_pa, u32 initrd_pa, u32 machine, u8 dry_run,
                                kernel path NUL, dtb path NUL, initrd path NUL -> text; then jumps */
#define BFP_STAGE     0x39   /* u8 index, u32 total, u32 offset, bytes -> u32 bytes held; boot path "mem:N" */
#define BFP_QUIT      0x70   /* agent exits and restores the previous USB client */
#define BFP_SETTIME   0x72   /* u32 UTC epoch -> u32 previous CE epoch (sets the CE clock/RTC) */
#define BFP_REBOOT    0x71   /* restart the device (SetSystemPowerState reset, then IOCTL_HAL_REBOOT) */
#define BFP_REGSET    0x21   /* u8 op (1 REG_SZ, 2 REG_MULTI_SZ, 3 REG_BINARY, 4 delete), name NUL, data;
                                only HKLM\\init Launch90/Depend90 (agent autostart) and the USB boot
                                default DefaultClientDriver = Mass_Storage_Class | Serial_Class */

/* status */
#define BFP_OK         0
#define BFP_E_UNKNOWN  1
#define BFP_E_ARGS     2
#define BFP_E_SYSTEM   3     /* payload: u32 GetLastError(), message */
#define BFP_E_DENIED   4
#define BFP_E_TOOBIG   5
#define BFP_E_CRC      6

static inline uint16_t bfp_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t bfp_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void bfp_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void bfp_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint16_t bfp_crc16(const uint8_t *data, size_t len);
void bfp_header(uint8_t *out, uint8_t type, uint16_t seq, uint16_t status, const uint8_t *payload, uint32_t len);

/* Incremental parser over a caller-provided buffer. */
typedef struct {
    uint8_t *buf;
    size_t used, cap;
    uint32_t dropped;
} bfp_parser;

typedef struct {
    uint8_t type;
    uint16_t seq, status;
    const uint8_t *payload;
    uint32_t len;
    int crc_ok;
} bfp_msg;

void bfp_parser_init(bfp_parser *p, uint8_t *buf, size_t cap);
size_t bfp_feed(bfp_parser *p, const uint8_t *data, size_t len);
/* 1 when a message (possibly with a bad payload CRC: msg.crc_ok == 0) is ready. */
int bfp_next(bfp_parser *p, bfp_msg *msg);
void bfp_consume(bfp_parser *p);

#endif
