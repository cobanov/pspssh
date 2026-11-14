/* pspssh — the screens you press buttons at.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host list and everything reached from it. This is the whole of the
 * application's interface before a session opens; afterwards the terminal has
 * the screen.
 */

#ifndef PSPSSH_UI_H
#define PSPSSH_UI_H

#include "hosts.h"

/* The list. Handles adding, editing and deleting on its own, and only returns
 * when there is nothing left to decide:
 *
 *   >= 0  connect to this host
 *    -1   the user is finished with the application
 */
int ui_host_list(void);

/* Something worth stopping for. `detail` may be NULL. Waits for a button. */
void ui_message(const char *title, const char *detail, unsigned int colour);

/* A yes or no, defaulting to no — every caller is about to destroy something. */
int ui_confirm(const char *question, const char *detail);

/* Asks for a password that was deliberately not stored. Returns 1 if one was
 * given, 0 if the user backed out. */
int ui_ask_password(const host_entry *entry, char *out, int out_len);

/* The top row every screen shares: a title on the left, and the build and whose
 * it is on the right. The terminal draws it too, in red while the bell rings.
 *
 * On a device flashed by hand the version is the answer to "did the new one
 * copy across?", and it is only useful where it is already being looked at. */
void ui_title_bar(const char *title, unsigned int colour);

#endif /* PSPSSH_UI_H */
