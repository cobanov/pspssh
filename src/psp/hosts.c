/* pspssh — the saved servers.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See hosts.h.
 */

#include "hosts.h"

#include "cardfile.h"
#include "wipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOSTS_PATH     "pspssh.hosts"
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

void hosts_forget_passwords(void)
{
    int i;

    for (i = 0; i < HOSTS_MAX; i++) {
        wipe_bytes(entries[i].password, sizeof(entries[i].password));
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

    if (cardfile_read(LEGACY_PATH, buffer, sizeof(buffer) - 1) < 0) {
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
    wipe_bytes(buffer, sizeof(buffer));
}

/* ----------------------------------------------------------------- save -- */

/* The whole list as text, then one atomic write.
 *
 * Building it in memory first is what lets cardfile own the atomicity: at
 * twenty-four entries this is under six kilobytes, and a partial write that
 * cannot happen is better than one that is handled. */
static int save(void)
{
    char text[HOSTS_MAX * (HOST_NAME_LEN + HOST_ADDRESS_LEN + HOST_USER_LEN
                           + HOST_PASSWORD_LEN + 32)];
    int at = 0;
    int i;
    int result;

    for (i = 0; i < count; i++) {
        int len = snprintf(text + at, sizeof(text) - (size_t)at,
                           "%s\t%s\t%d\t%s\t%s\t%d\n",
                           entries[i].name, entries[i].address, entries[i].port,
                           entries[i].user, entries[i].password,
                           entries[i].profile);

        if (len <= 0 || at + len >= (int)sizeof(text)) {
            wipe_bytes(text, sizeof(text));
            fail("the host list did not fit");
            return -1;
        }
        at += len;
    }

    result = cardfile_write(HOSTS_PATH, text, at);
    /* Passwords were in there. */
    wipe_bytes(text, sizeof(text));
    if (result != 0) {
        fail(cardfile_error());
    }
    return result;
}

/* ----------------------------------------------------------------- load -- */

int hosts_load(void)
{
    static char buffer[FILE_MAX];
    int len;

    count = 0;
    last_error[0] = '\0';

    len = cardfile_read(HOSTS_PATH, buffer, sizeof(buffer) - 1);
    if (len == -2) {
        fail(cardfile_error());
        return -1;
    }
    if (len < 0) {
        /* Nothing saved yet, so the old single-server config is imported if
         * there is one. */
        import_legacy();
        return 0;
    }

    parse(buffer);
    wipe_bytes(buffer, sizeof(buffer));
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
    wipe_bytes(entries[index].password, sizeof(entries[index].password));
    for (i = index; i + 1 < count; i++) {
        entries[i] = entries[i + 1];
    }
    count--;
    memset(&entries[count], 0, sizeof(entries[count]));
    return save();
}
