/* pspssh — remembering which key a server had.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See knownhosts.h.
 */

#include "knownhosts.h"
#include "cardfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KNOWN_PATH    "pspssh.known"

/* More addresses than there are host entries, because one machine reached by
 * name and by number is two records here — deliberately, since they are two
 * things a person could be attacked at independently. */
#define KNOWN_MAX     48
#define ADDRESS_LEN   96
#define FINGERPRINT_LEN 72

#define FILE_MAX (KNOWN_MAX * (ADDRESS_LEN + FINGERPRINT_LEN + 16))

typedef struct {
    char address[ADDRESS_LEN];
    int port;
    char fingerprint[FINGERPRINT_LEN];
} known_entry;

static known_entry entries[KNOWN_MAX];
static int count;
static char last_error[96];

static void fail(const char *message)
{
    strncpy(last_error, message, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

const char *knownhosts_error(void)
{
    return last_error[0] != '\0' ? last_error : "no error";
}

int knownhosts_count(void)
{
    return count;
}

static int find(const char *address, int port)
{
    int i;

    for (i = 0; i < count; i++) {
        if (entries[i].port == port
                && strcmp(entries[i].address, address) == 0) {
            return i;
        }
    }
    return -1;
}

const char *knownhosts_lookup(const char *address, int port)
{
    int at;

    if (address == NULL) {
        return NULL;
    }
    at = find(address, port);
    return at < 0 ? NULL : entries[at].fingerprint;
}

/* ------------------------------------------------------------------ file -- */

static int save(void)
{
    static char text[FILE_MAX];
    int at = 0;
    int i;

    for (i = 0; i < count; i++) {
        int len = snprintf(text + at, sizeof(text) - (size_t)at, "%s\t%d\t%s\n",
                           entries[i].address, entries[i].port,
                           entries[i].fingerprint);

        if (len <= 0 || at + len >= (int)sizeof(text)) {
            fail("the remembered keys did not fit");
            return -1;
        }
        at += len;
    }

    if (cardfile_write(KNOWN_PATH, text, at) != 0) {
        fail(cardfile_error());
        return -1;
    }
    return 0;
}

static void take_line(char *line)
{
    char *tab1;
    char *tab2;

    if (line[0] == '\0' || line[0] == '#' || count >= KNOWN_MAX) {
        return;
    }

    tab1 = strchr(line, '\t');
    if (tab1 == NULL) {
        return;
    }
    *tab1 = '\0';
    tab2 = strchr(tab1 + 1, '\t');
    if (tab2 == NULL) {
        return;
    }
    *tab2 = '\0';

    /* A record missing or overflowing any part is dropped rather than
     * half-loaded.
     *
     * Both failures point the same way. A remembered key with an empty
     * fingerprint matches nothing and reads as "this server's key changed" on
     * every connection, and an address silently cut to fit belongs to a
     * different server than the one written down — so each would raise an alarm
     * that is always wrong, which is worse than no alarm because it teaches
     * people to click through it. */
    if (line[0] == '\0' || tab2[1] == '\0') {
        return;
    }
    if (strlen(line) >= sizeof(entries[count].address)
            || strlen(tab2 + 1) >= sizeof(entries[count].fingerprint)) {
        return;
    }

    entries[count].port = atoi(tab1 + 1);
    if (entries[count].port < 1 || entries[count].port > 65535) {
        return;
    }
    strcpy(entries[count].address, line);
    strcpy(entries[count].fingerprint, tab2 + 1);
    count++;
}

int knownhosts_load(void)
{
    static char buffer[FILE_MAX];
    int len;
    char *line;

    count = 0;
    last_error[0] = '\0';

    len = cardfile_read(KNOWN_PATH, buffer, sizeof(buffer) - 1);
    if (len == -2) {
        fail(cardfile_error());
        return -1;
    }
    if (len < 0) {
        return 0;
    }

    line = buffer;
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
    return 0;
}

/* -------------------------------------------------------------- mutation -- */

int knownhosts_remember(const char *address, int port, const char *fingerprint)
{
    int at;

    if (address == NULL || fingerprint == NULL
            || address[0] == '\0' || fingerprint[0] == '\0') {
        fail("there is nothing to remember");
        return -1;
    }
    /* A tab or newline would read back as a field or record boundary, so a key
     * remembered for one address could come back attached to another. Nothing
     * here can contain one — fingerprints are base64 and addresses are typed on
     * a keyboard that has neither — which is exactly why it is cheap to check
     * rather than to assume. */
    if (strpbrk(address, "\t\r\n") != NULL
            || strpbrk(fingerprint, "\t\r\n") != NULL) {
        fail("that address cannot be stored");
        return -1;
    }

    at = find(address, port);
    if (at < 0) {
        if (count >= KNOWN_MAX) {
            fail("there is no room to remember another server");
            return -1;
        }
        at = count++;
    }

    snprintf(entries[at].address, sizeof(entries[at].address), "%s", address);
    entries[at].port = port;
    snprintf(entries[at].fingerprint, sizeof(entries[at].fingerprint), "%s",
             fingerprint);

    return save();
}

int knownhosts_forget(const char *address, int port)
{
    int at;
    int i;

    if (address == NULL) {
        return 0;
    }
    at = find(address, port);
    if (at < 0) {
        return 0;               /* already unknown, which is what was wanted */
    }

    for (i = at; i + 1 < count; i++) {
        entries[i] = entries[i + 1];
    }
    count--;
    memset(&entries[count], 0, sizeof(entries[count]));
    return save();
}
