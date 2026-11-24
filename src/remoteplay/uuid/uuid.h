/* uuid.h — o mínimo da libuuid que a libgamestream usa (uuid_generate_random e uuid_unparse),
 * com bytes de /dev/urandom. O FOSTON não tem a libuuid e não precisa dela para isso.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_UUID_SHIM_H
#define BF_UUID_SHIM_H

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned char uuid_t[16];
#define UUID_STRLEN 37

static inline void uuid_generate_random(uuid_t u)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, u, 16) : -1;
    if(fd >= 0) close(fd);
    if(n != 16) for(int i = 0; i < 16; i++) u[i] = (unsigned char)(i * 37 + 11);
    u[6] = (unsigned char)((u[6] & 0x0f) | 0x40);           /* versão 4 */
    u[8] = (unsigned char)((u[8] & 0x3f) | 0x80);           /* variante RFC 4122 */
}

static inline void uuid_unparse(const uuid_t u, char *out)
{
    snprintf(out, UUID_STRLEN, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

#endif
