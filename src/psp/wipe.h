/* pspssh — clearing a buffer that held something secret.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Three places hold a password at some point — the host store, the editor and
 * the session — and each of them wants the same six lines. They had two copies
 * between them and one place that used memset instead, which is the arrangement
 * where one of the three quietly stops clearing anything.
 *
 * memset is not enough on its own. A compiler is allowed to notice that nothing
 * reads the buffer afterwards and remove the call, and it is entitled to,
 * because the standard says nothing about a buffer's contents mattering after
 * its last read. Writing through a volatile pointer says they do.
 */

#ifndef PSPSSH_WIPE_H
#define PSPSSH_WIPE_H

static inline void wipe_bytes(void *data, unsigned int len)
{
    volatile unsigned char *at = (volatile unsigned char *)data;

    while (len-- > 0) {
        *at++ = 0;
    }
}

#endif /* PSPSSH_WIPE_H */
