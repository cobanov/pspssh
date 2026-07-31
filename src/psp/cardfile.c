/* pspssh — small files on the memory card.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See cardfile.h.
 */

#include "cardfile.h"

/* The memory card, or a directory on a laptop. src/host/psp_io_shim.h supplies
 * the six sceIo calls over POSIX so this and everything above it can be tested
 * without a PSP — which matters most here, where the failure mode is losing
 * somebody's data rather than drawing something wrong. */
#ifdef PSPSSH_HOST_TEST
#include "../host/psp_io_shim.h"
#else
#include <pspiofilemgr.h>
#endif

#include <stdio.h>
#include <string.h>

static char last_error[96];

static void fail(const char *message)
{
    strncpy(last_error, message, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

const char *cardfile_error(void)
{
    return last_error[0] != '\0' ? last_error : "no error";
}

void cardfile_wipe(void *data, unsigned int len)
{
    volatile unsigned char *at = (volatile unsigned char *)data;

    while (len-- > 0) {
        *at++ = 0;
    }
}

static void temp_name(const char *path, char *out, int out_len)
{
    snprintf(out, out_len, "%s.tmp", path);
}

static int read_file(const char *path, char *buffer, int max)
{
    SceUID fd;
    int total = 0;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return -1;
    }

    for (;;) {
        int n = sceIoRead(fd, buffer + total, max - total);

        if (n < 0) {
            sceIoClose(fd);
            fail("the file could not be read");
            return -2;
        }
        if (n == 0) {
            break;
        }
        total += n;
        if (total >= max) {
            sceIoClose(fd);
            /* Refused rather than truncated. Everything above this parses what
             * it is given as a complete file and writes it back. */
            fail("the file is larger than it should ever be");
            return -2;
        }
    }
    sceIoClose(fd);
    buffer[total] = '\0';
    return total;
}

int cardfile_read(const char *path, char *buffer, int max)
{
    char temp[64];
    int len;

    last_error[0] = '\0';

    len = read_file(path, buffer, max);
    if (len != -1) {
        return len;
    }

    /* No file. A write interrupted between removing the old one and renaming
     * the new one leaves it under the temporary name, so that is looked for
     * before concluding there is nothing. Starting empty there would throw away
     * everything to save the one entry being added. */
    temp_name(path, temp, sizeof(temp));
    len = read_file(temp, buffer, max);
    if (len < 0) {
        return -1;
    }
    sceIoRename(temp, path);
    return len;
}

int cardfile_write(const char *path, const char *text, int len)
{
    char temp[64];
    SceUID fd;
    int written = 0;

    last_error[0] = '\0';
    temp_name(path, temp, sizeof(temp));

    fd = sceIoOpen(temp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        fail("the file could not be opened for writing");
        return -1;
    }

    while (written < len) {
        int n = sceIoWrite(fd, text + written, len - written);

        if (n <= 0) {
            sceIoClose(fd);
            sceIoRemove(temp);
            fail("the file could not be written");
            return -1;
        }
        written += n;
    }
    sceIoClose(fd);

    /* Rename cannot replace an existing file on this filesystem, so the old one
     * goes first. That is the one moment neither file is in place, and
     * cardfile_read recovers from it. */
    sceIoRemove(path);
    if (sceIoRename(temp, path) < 0) {
        fail("the file was written but could not be moved into place");
        return -1;
    }
    return 0;
}
