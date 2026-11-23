/* out.h — eventos para o shell, uma linha por evento no stdout. SPDX-License-Identifier: GPL-2.0-only */
#ifndef BIGFOSTON_OUT_H
#define BIGFOSTON_OUT_H
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
