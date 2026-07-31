/* pspssh — the saved servers.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The list the application opens on. Until there was a keyboard this was a
 * single server in a text file, and reaching a different machine meant
 * unplugging the console and editing that file on a computer.
 *
 * ## Where it lives
 *
 * `pspssh.hosts`, next to the binary, one record per line:
 *
 *     name<TAB>address<TAB>port<TAB>user<TAB>password<TAB>profile
 *
 * Tab separated because tabs are the one character the on-screen keyboard
 * cannot produce, so no name typed on the device can forge a field boundary.
 * Entries are still checked on the way in rather than trusted — a file edited
 * on a computer is not typed on the device.
 *
 * ## The password is in plain text
 *
 * As it was in pspssh.cfg, and worth saying rather than hiding: anyone holding
 * the console can read it. It is now optional — an entry with no password asks
 * at connect time — which is the part that actually needed a keyboard.
 */

#ifndef PSPSSH_HOSTS_H
#define PSPSSH_HOSTS_H

#define HOSTS_MAX        24
#define HOST_NAME_LEN    24
#define HOST_ADDRESS_LEN 96
#define HOST_USER_LEN    32
#define HOST_PASSWORD_LEN 64

typedef struct {
    char name[HOST_NAME_LEN];
    char address[HOST_ADDRESS_LEN];
    char user[HOST_USER_LEN];
    char password[HOST_PASSWORD_LEN];   /* empty means "ask me" */
    int port;
    int profile;                        /* 0 means "try whichever works" */
} host_entry;

/* Reads the file, importing an old pspssh.cfg if there is one and no host file
 * yet. Returns 0 on success, or -1 if the file exists but could not be read in
 * full — in which case nothing is loaded and the caller must say so rather than
 * carry on with a fragment of somebody's list. */
int hosts_load(void);

/* Why the last load or save failed, for showing on screen. Never NULL. */
const char *hosts_error(void);

int hosts_count(void);

/* NULL if the index is out of range. The pointer is into the store, so it stays
 * valid until the list is next changed. */
const host_entry *hosts_at(int index);

/* All three save immediately. Returns 0 on success.
 *
 * hosts_add returns the new index, or -1 if the list is full or the entry is
 * not usable. */
int hosts_add(const host_entry *entry);
int hosts_replace(int index, const host_entry *entry);
int hosts_remove(int index);

/* Whether an entry has the fields a connection needs, and none that would
 * corrupt the file. Returns NULL if it is fine, or a sentence saying what is
 * wrong. */
const char *hosts_problem(const host_entry *entry);

/* Fills in the defaults a new entry starts from. */
void hosts_blank(host_entry *entry);

/* Wipes every stored password from memory. For the way out, so a credential
 * read off the card does not outlive the session that used it. */
void hosts_forget_passwords(void);

#endif /* PSPSSH_HOSTS_H */
