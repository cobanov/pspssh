/* pspssh — remembering which key a server had.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Until this existed the client printed the server's fingerprint and connected
 * regardless. That is not a check, it is a caption — and a caption nobody
 * compares against anything is worse than none, because it looks like a check.
 * Anything on the same Wi-Fi could sit between the console and the server, and
 * the client would encrypt everything perfectly to the wrong party.
 *
 * ## Trust on first use
 *
 * The same bargain OpenSSH makes. The first connection to an address is taken
 * on trust — the user is shown the fingerprint and asked — and every connection
 * after that must present the same key. A change is refused rather than warned
 * about: there is no "yes, continue anyway", because on a handheld that button
 * is pressed reflexively and it is the only moment the attack is visible.
 *
 * A key can be forgotten deliberately, from the host list. Servers really are
 * rebuilt, and an entry that could never be connected to again would just teach
 * people to delete the whole host and re-add it — which forgets the key anyway,
 * with less thought.
 *
 * ## What is stored
 *
 * `pspssh.known`, beside the binary:
 *
 *     address<TAB>port<TAB>SHA256:base64
 *
 * The fingerprint rather than the key: it is what the user is shown, it is what
 * `ssh-keygen -lf` prints, and forty-three characters compare exactly as well
 * as the blob they came from.
 *
 * Keyed by address and port, not by host name — two saved entries pointing at
 * the same machine should agree about its key, and renaming one should not
 * make it a stranger.
 */

#ifndef PSPSSH_KNOWNHOSTS_H
#define PSPSSH_KNOWNHOSTS_H

/* Reads the file. Returns 0, or -1 if it exists and could not be read whole —
 * in which case nothing is loaded, because a list of trusted keys read short is
 * a list that silently trusts less than it should. */
int knownhosts_load(void);

/* The fingerprint remembered for this address, or NULL if it is new. */
const char *knownhosts_lookup(const char *address, int port);

/* Remembers one, replacing any earlier fingerprint for the same address.
 * Returns 0 on success. */
int knownhosts_remember(const char *address, int port, const char *fingerprint);

/* Forgets one. Returns 0 whether or not there was anything to forget: the
 * caller wants the address to be unknown afterwards, and it is. */
int knownhosts_forget(const char *address, int port);

int knownhosts_count(void);

/* Why the last call failed. Never NULL. */
const char *knownhosts_error(void);

#endif /* PSPSSH_KNOWNHOSTS_H */
