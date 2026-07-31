/* pspssh — small files on the memory card, read whole and written atomically.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two things keep data on the card — the saved hosts and the remembered host
 * keys — and both need the same two properties, for the same reasons:
 *
 * **Read whole or not at all.** The reader this replaced took the first 1023
 * bytes and parsed whatever landed, so a longer file lost its tail mid-line.
 * A list read short is worse than a list not read: it gets written back, and
 * the missing half is gone for good.
 *
 * **Written atomically.** A battery pull halfway through adding one server
 * should not cost all the others. The new content goes beside the real file and
 * is moved into place, and the one moment neither is there is recovered on the
 * next read.
 */

#ifndef PSPSSH_CARDFILE_H
#define PSPSSH_CARDFILE_H

/* Reads `path` in full, terminating what it read.
 *
 * If `path` is absent but an interrupted write left `path.tmp` behind, that is
 * used instead and moved into place — so a save that died between removing the
 * old file and renaming the new one loses nothing.
 *
 * Returns the length, -1 if there is no such file, or -2 if there is one and it
 * could not be read whole. A caller seeing -2 must not proceed with what it
 * got: there is nothing there.
 */
int cardfile_read(const char *path, char *buffer, int max);

/* Writes `len` bytes to `path.tmp` and moves it over `path`. Returns 0. */
int cardfile_write(const char *path, const char *text, int len);

/* Why the last call failed. Never NULL. */
const char *cardfile_error(void);

#endif /* PSPSSH_CARDFILE_H */
