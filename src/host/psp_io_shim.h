/* pspssh — the PSP's file calls, on a laptop.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Just enough of pspiofilemgr.h to compile and run src/psp/hosts.c off the
 * device. It exists so the host list can be tested — round trips, refusals,
 * the atomic save — rather than trusted, because the failure it guards against
 * is silently losing every server somebody saved.
 *
 * Only the six calls hosts.c makes, and no attempt to be a general shim: a
 * fake that covered more than the thing it stands in for would be a second
 * implementation to keep honest.
 */

#ifndef PSPSSH_PSP_IO_SHIM_H
#define PSPSSH_PSP_IO_SHIM_H

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

typedef int SceUID;

#define PSP_O_RDONLY O_RDONLY
#define PSP_O_WRONLY O_WRONLY
#define PSP_O_CREAT  O_CREAT
#define PSP_O_TRUNC  O_TRUNC

static inline SceUID sceIoOpen(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}

static inline int sceIoRead(SceUID fd, void *buf, int len)
{
    return (int)read(fd, buf, (size_t)len);
}

static inline int sceIoWrite(SceUID fd, const void *buf, int len)
{
    return (int)write(fd, buf, (size_t)len);
}

static inline int sceIoClose(SceUID fd)
{
    return close(fd);
}

static inline int sceIoRemove(const char *path)
{
    return remove(path);
}

static inline int sceIoRename(const char *from, const char *to)
{
    return rename(from, to);
}

#endif /* PSPSSH_PSP_IO_SHIM_H */
