/* pspssh — the saved servers.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See hosts.h.
 */

#include "hosts.h"

/* The memory card, or a directory on a laptop.
 *
 * hosts.c is the one PSP module with logic worth testing away from a PSP —
 * parsing, validation and the atomic save are all decidable on a host, and
 * finding out that a host list round-trips wrongly by losing somebody's servers
 * is a bad way to find out. src/host/psp_io_shim.h supplies the six sceIo
 * calls over POSIX so tools/test-host.sh can drive the real code. */
#ifdef PSPSSH_HOST_TEST
#include "../host/psp_io_shim.h"
#else
#include <pspiofilemgr.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOSTS_PATH     "pspssh.hosts"
#define HOSTS_TEMP     "pspssh.hosts.tmp"
#define LEGACY_PATH    "pspssh.cfg"

/* Every field of every record, plus separators and a generous margin. A file
 * larger than this is not a host list that grew; it is a different file, or a
 * corrupted one, and reading the first 16 KB of it would be worse than
 * refusing. */
#define FILE_MAX 16384

/* Defined below, and needed by the legacy import above it. */
static int save(void);

static host_entry entries[HOSTS_MAX];
static int count;
static char last_error[128];

static void fail(const char *message)
{
    strncpy(last_error, message, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

const char *hosts_error(void)
{
    return last_error[0] != '\0' ? last_error : "no error";
}

int hosts_count(void)
{
    return count;
}

const host_entry *hosts_at(int index)
{
    if (index < 0 || index >= count) {
        return NULL;
    }
    return &entries[index];
}

void hosts_blank(host_entry *entry)
{
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->port = 22;
    entry->profile = 0;
}

/* Overwriting through a volatile pointer, so the compiler cannot decide that a
 * buffer nobody reads again does not need clearing — which is exactly the
 * optimisation that turns a memset of a dead password into nothing at all. */
static void wipe(void *data, unsigned int len)
{
    volatile unsigned char *at = (volatile unsigned char *)data;

    while (len-- > 0) {
        *at++ = 0;
    }
}

void hosts_forget_passwords(void)
{
    int i;

    for (i = 0; i < HOSTS_MAX; i++) {
        wipe(entries[i].password, sizeof(entries[i].password));
    }
}

const char *hosts_problem(const host_entry *entry)
{
    if (entry == NULL) {
        return "there is nothing to save";
    }
    if (entry->address[0] == '\0') {
        return "a host needs an address";
    }
    if (entry->user[0] == '\0') {
        return "a host needs a user name";
    }
    if (entry->port < 1 || entry->port > 65535) {
        return "a port has to be between 1 and 65535";
    }
    if (entry->profile < 0 || entry->profile > 10) {
        return "a wi-fi profile is 1 to 10, or 0 for any";
    }
    /* A tab or a newline in a field would read back as a different record — or
     * as several. The keyboard cannot produce either, so this is about files
     * edited on a computer, and about not trusting that they were not. */
    if (strpbrk(entry->name, "\t\r\n") != NULL
            || strpbrk(entry->address, "\t\r\n") != NULL
            || strpbrk(entry->user, "\t\r\n") != NULL
            || strpbrk(entry->password, "\t\r\n") != NULL) {
        return "tabs and line breaks cannot be stored in a field";
    }
    return NULL;
}

/* ----------------------------------------------------------------- read -- */

static void copy_field(char *to, int to_len, const char *from)
{
    int i = 0;

    while (from[i] != '\0' && i < to_len - 1) {
        to[i] = from[i];
        i++;
    }
    to[i] = '\0';
}

/* Splits `line` at tabs in place, returning how many fields were found. */
static int split(char *line, char **fields, int max)
{
    int found = 0;

    fields[found++] = line;
    while (*line != '\0' && found < max) {
        if (*line == '\t') {
            *line = '\0';
            fields[found++] = line + 1;
        }
        line++;
    }
    return found;
}

static void take_line(char *line)
{
    char *fields[8];
    int found;
    host_entry entry;

    if (line[0] == '\0' || line[0] == '#') {
        return;
    }
    if (count >= HOSTS_MAX) {
        return;
    }

    found = split(line, fields, 8);
    if (found < 6) {
        return;
    }

    hosts_blank(&entry);
    copy_field(entry.name, sizeof(entry.name), fields[0]);
    copy_field(entry.address, sizeof(entry.address), fields[1]);
    entry.port = atoi(fields[2]);
    copy_field(entry.user, sizeof(entry.user), fields[3]);
    copy_field(entry.password, sizeof(entry.password), fields[4]);
    entry.profile = atoi(fields[5]);

    /* A record that could not be connected to is dropped rather than shown.
     * Half a host in the list is an invitation to press X on it. */
    if (hosts_problem(&entry) != NULL) {
        return;
    }
    entries[count++] = entry;
}

/* Reads a whole file, or reports that it could not.
 *
 * The old config reader took the first 1023 bytes and parsed whatever landed,
 * so a longer file lost its tail mid-line — and `password=hunter2seventeen`
 * cut to `password=hunter2` is an authentication failure whose cause is
 * invisible, because the file on the card plainly says the right thing. With a
 * list that grows by an entry every time somebody adds a server, that stopped
 * being theoretical. */
static int read_whole(const char *path, char *buffer, int max)
{
    SceUID fd;
    int total = 0;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return -1;          /* absent is not an error; the caller decides */
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
            fail("the file is too large to be a host list");
            return -2;
        }
    }
    sceIoClose(fd);
    buffer[total] = '\0';
    return total;
}

static void parse(char *text)
{
    char *line = text;

    while (*line != '\0') {
        char *end = line;

        while (*end != '\0' && *end != '\n' && *end != '\r') {
            end++;
        }
        while (*end == '\n' || *end == '\r') {
            *end++ = '\0';
        }
        take_line(line);
        line = end;
    }
}

/* ------------------------------------------------------------- legacy -- */

/* The single server from pspssh.cfg, so upgrading does not lose it.
 *
 * Read once, when there is no host file yet. The old file is neither rewritten
 * nor deleted: somebody put it there by hand and it is theirs. */
static void import_legacy(void)
{
    static char buffer[FILE_MAX];
    host_entry entry;
    char *line;

    if (read_whole(LEGACY_PATH, buffer, sizeof(buffer) - 1) < 0) {
        return;
    }

    hosts_blank(&entry);
    line = buffer;
    while (*line != '\0') {
        char *end = line;
        char *eq;

        while (*end != '\0' && *end != '\n' && *end != '\r') {
            end++;
        }
        while (*end == '\n' || *end == '\r') {
            *end++ = '\0';
        }

        eq = strchr(line, '=');
        if (line[0] != '#' && eq != NULL) {
            char *key = line;
            char *value = eq + 1;

            *eq = '\0';
            while (*key == ' ') key++;
            while (*value == ' ') value++;

            if (strcmp(key, "host") == 0) {
                copy_field(entry.address, sizeof(entry.address), value);
            } else if (strcmp(key, "port") == 0) {
                entry.port = atoi(value);
            } else if (strcmp(key, "user") == 0) {
                copy_field(entry.user, sizeof(entry.user), value);
            } else if (strcmp(key, "password") == 0) {
                copy_field(entry.password, sizeof(entry.password), value);
            } else if (strcmp(key, "profile") == 0) {
                entry.profile = atoi(value);
            }
        }
        line = end;
    }

    if (entry.port <= 0) {
        entry.port = 22;
    }
    copy_field(entry.name, sizeof(entry.name), entry.address);
    if (hosts_problem(&entry) == NULL) {
        entries[count++] = entry;
        save();
    }
    wipe(buffer, sizeof(buffer));
}

/* ----------------------------------------------------------------- save -- */

static int write_all(SceUID fd, const char *text, int len)
{
    int written = 0;

    while (written < len) {
        int n = sceIoWrite(fd, text + written, len - written);

        if (n <= 0) {
            return -1;
        }
        written += n;
    }
    return 0;
}

static int save(void)
{
    SceUID fd;
    char line[HOST_NAME_LEN + HOST_ADDRESS_LEN + HOST_USER_LEN
              + HOST_PASSWORD_LEN + 32];
    int i;

    /* Written beside the real file and moved into place, so a battery pull
     * halfway through a write cannot leave a truncated list. The failure this
     * avoids is losing every saved server while adding one. */
    fd = sceIoOpen(HOSTS_TEMP, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        fail("the host list could not be opened for writing");
        return -1;
    }

    for (i = 0; i < count; i++) {
        int len = snprintf(line, sizeof(line), "%s\t%s\t%d\t%s\t%s\t%d\n",
                           entries[i].name, entries[i].address, entries[i].port,
                           entries[i].user, entries[i].password,
                           entries[i].profile);

        if (len <= 0 || len >= (int)sizeof(line)
                || write_all(fd, line, len) != 0) {
            sceIoClose(fd);
            sceIoRemove(HOSTS_TEMP);
            wipe(line, sizeof(line));
            fail("the host list could not be written");
            return -1;
        }
    }
    sceIoClose(fd);
    wipe(line, sizeof(line));

    /* Rename cannot replace an existing file on this filesystem, so the old one
     * goes first. That is the one moment where neither file is in place; if the
     * rename then fails, the temporary file is still there and hosts_load picks
     * it up next time rather than starting empty. */
    sceIoRemove(HOSTS_PATH);
    if (sceIoRename(HOSTS_TEMP, HOSTS_PATH) < 0) {
        fail("the host list was written but could not be moved into place");
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------- load -- */

int hosts_load(void)
{
    static char buffer[FILE_MAX];
    int len;

    count = 0;
    last_error[0] = '\0';

    len = read_whole(HOSTS_PATH, buffer, sizeof(buffer) - 1);
    if (len == -2) {
        return -1;
    }
    if (len < 0) {
        /* No host file. A save that was interrupted after the old file was
         * removed leaves the new one under its temporary name, so that is
         * looked for before falling back to the old single-server config. */
        len = read_whole(HOSTS_TEMP, buffer, sizeof(buffer) - 1);
        if (len >= 0) {
            parse(buffer);
            wipe(buffer, sizeof(buffer));
            save();
            return 0;
        }
        import_legacy();
        return 0;
    }

    parse(buffer);
    wipe(buffer, sizeof(buffer));
    return 0;
}

/* ------------------------------------------------------------- mutation -- */

int hosts_add(const host_entry *entry)
{
    if (count >= HOSTS_MAX) {
        fail("there is no room for another host");
        return -1;
    }
    if (hosts_problem(entry) != NULL) {
        fail(hosts_problem(entry));
        return -1;
    }
    entries[count] = *entry;
    count++;
    if (save() != 0) {
        return -1;
    }
    return count - 1;
}

int hosts_replace(int index, const host_entry *entry)
{
    if (index < 0 || index >= count) {
        fail("that host is no longer in the list");
        return -1;
    }
    if (hosts_problem(entry) != NULL) {
        fail(hosts_problem(entry));
        return -1;
    }
    entries[index] = *entry;
    return save();
}

int hosts_remove(int index)
{
    int i;

    if (index < 0 || index >= count) {
        fail("that host is no longer in the list");
        return -1;
    }
    wipe(entries[index].password, sizeof(entries[index].password));
    for (i = index; i + 1 < count; i++) {
        entries[i] = entries[i + 1];
    }
    count--;
    memset(&entries[count], 0, sizeof(entries[count]));
    return save();
}
