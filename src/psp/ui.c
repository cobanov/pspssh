/* pspssh — the screens you press buttons at.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ui.h.
 *
 * X confirms and O goes back, everywhere, including on a console whose system
 * software has them the other way round. The button-swap setting is honoured
 * where it belongs — inside the system keyboard, which reads it itself — and
 * deliberately not here: homebrew has used X for confirm since before there was
 * a setting to consult, and a client that disagreed with every other
 * application on the memory card would be the odd one out rather than the
 * correct one.
 */

#include "ui.h"
#include "gfx.h"
#include "knownhosts.h"
#include "osk.h"
#include "pad.h"

#include "../core/pspssh.h"

#include <pspctrl.h>
#include <pspkernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_TOP     4
/* Derived rather than chosen, so it cannot outlive the screen it was sized
 * for. The three rows below it are the scroll indicator, the footer, and the
 * one the footer needs beneath it. */
#define LIST_ROWS    (GFX_ROWS - LIST_TOP - 3)
#define FOOTER_ROW   (GFX_ROWS - 2)

/* ------------------------------------------------------------------ frame -- */

void ui_frame(const char *title, const char *legend)
{
    gfx_clear(GFX_BLACK);

    gfx_fill_cells(0, 0, GFX_COLS, 1, GFX_ACCENT);
    gfx_printf(1, 0, GFX_WHITE, GFX_ACCENT, "%s", title);
    /* The build and whose it is, on every screen. On a device flashed by hand
     * the version is the answer to "did the new one copy across?", and it is
     * only useful where it is already being looked at. */
    gfx_printf(GFX_COLS - 26, 0, GFX_WHITE, GFX_ACCENT,
               "v%s  %s", PSPSSH_VERSION, PSPSSH_AUTHOR);

    if (legend != NULL) {
        gfx_fill_cells(0, FOOTER_ROW, GFX_COLS, 1, GFX_BLACK);
        gfx_puts(1, FOOTER_ROW, legend, GFX_GREY, GFX_BLACK);
    }
}

static void wait_for_release(void)
{
    /* Drains the button that got us here. Without it a press is read again by
     * whatever screen comes next, so confirming a dialog immediately confirms
     * the one behind it. */
    int frames = 0;

    while (pad_held() != 0 && frames++ < 60) {
        pad_pressed();
        sceKernelDelayThread(16 * 1000);
    }
}

/* ---------------------------------------------------------------- message -- */

void ui_message(const char *title, const char *detail, unsigned int colour)
{
    wait_for_release();

    for (;;) {
        unsigned int pressed;

        ui_frame("pspssh", "X  continue");
        gfx_puts(2, LIST_TOP + 2, title, colour, GFX_BLACK);
        if (detail != NULL) {
            gfx_puts(2, LIST_TOP + 4, detail, GFX_GREY, GFX_BLACK);
        }
        gfx_flip();

        pressed = pad_pressed();
        if ((pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) != 0
                || pad_exit_requested()) {
            break;
        }
    }
    wait_for_release();
}

int ui_confirm(const char *question, const char *detail)
{
    wait_for_release();

    for (;;) {
        unsigned int pressed;

        ui_frame("pspssh", "X  yes            O  no");
        gfx_puts(2, LIST_TOP + 2, question, GFX_YELLOW, GFX_BLACK);
        if (detail != NULL) {
            gfx_puts(2, LIST_TOP + 4, detail, GFX_GREY, GFX_BLACK);
        }
        gfx_flip();

        pressed = pad_pressed();
        if (pad_exit_requested() || (pressed & PSP_CTRL_CIRCLE) != 0) {
            wait_for_release();
            return 0;
        }
        if ((pressed & PSP_CTRL_CROSS) != 0) {
            wait_for_release();
            return 1;
        }
    }
}

/* ------------------------------------------------------------------ editor -- */

/* The form being edited, at file scope because the on-screen keyboard paints
 * the backdrop through a callback while it is up. */
static host_entry form;
static int form_field;
static int form_is_new;

#define FIELD_NAME     0
#define FIELD_ADDRESS  1
#define FIELD_PORT     2
#define FIELD_USER     3
#define FIELD_PASSWORD 4
#define FIELD_PROFILE  5
#define FIELD_COUNT    6

static const char *field_labels[FIELD_COUNT] = {
    "name", "address", "port", "user", "password", "wi-fi profile"
};

static void field_value(int field, char *out, int out_len)
{
    switch (field) {
    case FIELD_NAME:
        snprintf(out, out_len, "%s", form.name[0] ? form.name : "(same as address)");
        break;
    case FIELD_ADDRESS:
        snprintf(out, out_len, "%s", form.address[0] ? form.address : "(needed)");
        break;
    case FIELD_PORT:
        snprintf(out, out_len, "%d", form.port);
        break;
    case FIELD_USER:
        snprintf(out, out_len, "%s", form.user[0] ? form.user : "(needed)");
        break;
    case FIELD_PASSWORD:
        /* Never shown, not even as the right number of asterisks. Someone
         * reading over a shoulder should not learn the length. */
        snprintf(out, out_len, "%s",
                 form.password[0] ? "(stored on the card)" : "(ask each time)");
        break;
    case FIELD_PROFILE:
        if (form.profile == 0) {
            snprintf(out, out_len, "any that works");
        } else {
            snprintf(out, out_len, "%d", form.profile);
        }
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void draw_form(void *ctx)
{
    int i;

    (void)ctx;
    ui_frame(form_is_new ? "add a host" : "edit host",
             "X  change      []  save      O  cancel");

    for (i = 0; i < FIELD_COUNT; i++) {
        /* One row a field, not two. Twelve rows of double spacing fitted a
         * 34-row screen and does not fit a 22-row one. */
        int row = LIST_TOP + i;
        int chosen = (i == form_field);
        unsigned int bg = chosen ? GFX_ACCENT : GFX_BLACK;
        char value[HOST_ADDRESS_LEN + 8];

        field_value(i, value, sizeof(value));
        gfx_fill_cells(1, row, GFX_COLS - 2, 1, bg);
        gfx_puts(2, row, chosen ? ">" : " ", GFX_WHITE, bg);
        gfx_printf(4, row, GFX_GREY, bg, "%-14s", field_labels[i]);
        gfx_puts(20, row, value, GFX_WHITE, bg);
    }

    if (form.password[0] != '\0') {
        gfx_puts(2, LIST_TOP + FIELD_COUNT + 2,
                 "a stored password is plain text on the memory card",
                 GFX_DIM, GFX_BLACK);
    }
}

/* A name for a host the user did not name, derived from its address.
 *
 * The first label rather than the first 23 characters: "pve" is a better row in
 * the list than "pve.internal.example.o", and truncating in the middle of a
 * domain produces something that looks like a typo. An address that is really
 * an address keeps all of itself, since 192.168.8.45 has no label to take and
 * fits anyway. */
static void name_from_address(void)
{
    const char *from = form.address;
    int numeric = (from[0] >= '0' && from[0] <= '9');
    int i = 0;

    while (from[i] != '\0' && i < (int)sizeof(form.name) - 1) {
        if (!numeric && from[i] == '.') {
            break;
        }
        form.name[i] = from[i];
        i++;
    }
    form.name[i] = '\0';
}

/* Shows a keyboard that would not open, rather than letting the button look
 * broken. Returns what osk_prompt returned, so callers can go on ignoring
 * OSK_CANCELLED and stop ignoring OSK_UNAVAILABLE. */
static osk_result ask(const char *prompt, const char *initial,
                      char *out, int out_len, osk_kind kind)
{
    osk_result result = osk_prompt(prompt, initial, out, out_len, kind,
                                   draw_form, NULL);

    if (result == OSK_UNAVAILABLE) {
        char detail[64];

        snprintf(detail, sizeof(detail),
                 "sceUtilityOskInitStart gave 0x%08x", osk_start_error());
        ui_message("the keyboard would not open", detail, GFX_RED);
    }
    return result;
}

static void edit_current_field(void)
{
    /* The field is passed as both the initial value and the destination.
     * osk_prompt leaves the destination untouched when the user cancels, so
     * "cancel" keeps what was there — and the buffer the keyboard is limited to
     * is the buffer it writes into, which is the same number stated once
     * instead of two that have to agree. */
    switch (form_field) {
    case FIELD_NAME:
        ask("a short name for this host", form.name,
            form.name, sizeof(form.name), OSK_TEXT);
        break;

    case FIELD_ADDRESS:
        ask("host name or ip address", form.address,
            form.address, sizeof(form.address), OSK_HOSTNAME);
        break;

    case FIELD_USER:
        ask("user name on the server", form.user,
            form.user, sizeof(form.user), OSK_TEXT);
        break;

    case FIELD_PASSWORD:
        /* Never pre-filled with the stored one: the panel would show it in
         * clear text on a screen somebody else can see. An empty prompt that
         * keeps the old value when cancelled is the safer default. */
        ask("password — leave empty to be asked each time", "",
            form.password, sizeof(form.password), OSK_TEXT);
        break;

    case FIELD_PORT: {
        char digits[8];

        snprintf(digits, sizeof(digits), "%d", form.port);
        if (ask("port", digits, digits, sizeof(digits), OSK_DIGITS)
                == OSK_ENTERED) {
            int value = atoi(digits);

            /* Kept only if it is a port. Rejecting here means the save button
             * is never the place someone learns they typed a letter. */
            if (value >= 1 && value <= 65535) {
                form.port = value;
            } else {
                ui_message("that is not a port",
                           "a port is a number from 1 to 65535", GFX_RED);
            }
        }
        break;
    }

    case FIELD_PROFILE: {
        char digits[8];

        snprintf(digits, sizeof(digits), "%d", form.profile);
        if (ask("wi-fi profile number, or 0 to try each one", digits,
                digits, sizeof(digits), OSK_DIGITS) == OSK_ENTERED) {
            int value = atoi(digits);

            if (value >= 0 && value <= 10) {
                form.profile = value;
            } else {
                ui_message("that is not a profile",
                           "the psp holds ten, numbered 1 to 10", GFX_RED);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* Returns 1 if the entry was saved. */
static int edit_host(int index)
{
    const host_entry *existing = hosts_at(index);

    form_is_new = (existing == NULL);
    if (existing != NULL) {
        form = *existing;
    } else {
        hosts_blank(&form);
    }
    form_field = form_is_new ? FIELD_ADDRESS : FIELD_NAME;

    wait_for_release();

    for (;;) {
        unsigned int pressed;

        draw_form(NULL);
        gfx_flip();

        if (pad_exit_requested()) {
            return 0;
        }
        pressed = pad_pressed();

        if ((pressed & PSP_CTRL_UP) != 0) {
            form_field = (form_field + FIELD_COUNT - 1) % FIELD_COUNT;
        } else if ((pressed & PSP_CTRL_DOWN) != 0) {
            form_field = (form_field + 1) % FIELD_COUNT;
        } else if ((pressed & PSP_CTRL_CROSS) != 0) {
            edit_current_field();
            wait_for_release();
        } else if ((pressed & PSP_CTRL_SQUARE) != 0) {
            const char *problem;

            /* An unnamed host is named after its address, because a blank row
             * in the list is worse than a repetitive one. */
            if (form.name[0] == '\0') {
                name_from_address();
            }
            problem = hosts_problem(&form);
            if (problem != NULL) {
                ui_message("that host cannot be saved", problem, GFX_RED);
                continue;
            }
            if (form_is_new) {
                if (hosts_add(&form) < 0) {
                    ui_message("it could not be saved", hosts_error(), GFX_RED);
                    continue;
                }
            } else if (hosts_replace(index, &form) != 0) {
                ui_message("it could not be saved", hosts_error(), GFX_RED);
                continue;
            }
            memset(&form, 0, sizeof(form));
            return 1;
        } else if ((pressed & PSP_CTRL_CIRCLE) != 0) {
            memset(&form, 0, sizeof(form));
            wait_for_release();
            return 0;
        }
    }
}

/* -------------------------------------------------------------- password -- */

int ui_ask_password(const host_entry *entry, char *out, int out_len)
{
    char prompt[HOST_USER_LEN + HOST_ADDRESS_LEN + 16];

    snprintf(prompt, sizeof(prompt), "password for %s@%s",
             entry->user, entry->address);

    wait_for_release();
    if (osk_prompt(prompt, "", out, out_len, OSK_TEXT, NULL, NULL)
            != OSK_ENTERED) {
        if (osk_start_error() != 0) {
            char detail[64];

            snprintf(detail, sizeof(detail),
                     "sceUtilityOskInitStart gave 0x%08x", osk_start_error());
            ui_message("the keyboard would not open", detail, GFX_RED);
        }
        return 0;
    }
    return out[0] != '\0';
}

/* ------------------------------------------------------------------ list -- */

static void draw_list(int selected, int top)
{
    int count = hosts_count();
    int shown;
    int i;

    ui_frame("pspssh",
             "X connect  [] edit  /\\ delete  SEL forget key  O quit");

    if (count == 0) {
        gfx_puts(4, LIST_TOP + 1, "no hosts saved yet.",
                 GFX_WHITE, GFX_BLACK);
        gfx_puts(4, LIST_TOP + 3, "press X to add one — the on-screen keyboard",
                 GFX_GREY, GFX_BLACK);
        gfx_puts(4, LIST_TOP + 4, "will ask for the address and the user name.",
                 GFX_GREY, GFX_BLACK);
    }

    shown = count + 1;              /* the "add a host" row lives at the end */
    for (i = 0; i < LIST_ROWS && top + i < shown; i++) {
        int index = top + i;
        int row = LIST_TOP + i;
        int chosen = (index == selected);
        unsigned int bg = chosen ? GFX_ACCENT : GFX_BLACK;

        gfx_fill_cells(1, row, GFX_COLS - 2, 1, bg);
        gfx_puts(2, row, chosen ? ">" : " ", GFX_WHITE, bg);

        if (index == count) {
            gfx_puts(4, row, "+  add a host", GFX_GREEN, bg);
        } else {
            const host_entry *entry = hosts_at(index);

            gfx_printf(4, row, GFX_WHITE, bg, "%-23s", entry->name);
            gfx_printf(28, row, GFX_GREY, bg, "%s@%s:%d",
                       entry->user, entry->address, entry->port);
        }
    }

    if (shown > LIST_ROWS) {
        gfx_printf(GFX_COLS - 12, LIST_TOP + LIST_ROWS, GFX_DIM, GFX_BLACK,
                   "%d of %d", selected + 1, shown);
    }
}

int ui_host_list(void)
{
    int selected = 0;
    int top = 0;

    for (;;) {
        int count = hosts_count();
        int shown = count + 1;
        unsigned int pressed;

        /* The list changes underneath this — a delete can take the selected row
         * away — so it is clamped every frame rather than at the point of
         * change, where one path out of three would eventually forget. */
        if (selected >= shown) {
            selected = shown - 1;
        }
        if (selected < 0) {
            selected = 0;
        }
        if (selected < top) {
            top = selected;
        }
        if (selected >= top + LIST_ROWS) {
            top = selected - LIST_ROWS + 1;
        }

        draw_list(selected, top);
        gfx_flip();

        if (pad_exit_requested()) {
            return -1;
        }
        pressed = pad_pressed();

        if ((pressed & PSP_CTRL_UP) != 0) {
            selected = (selected + shown - 1) % shown;
        } else if ((pressed & PSP_CTRL_DOWN) != 0) {
            selected = (selected + 1) % shown;
        } else if ((pressed & PSP_CTRL_CROSS) != 0) {
            if (selected == count) {
                edit_host(-1);
                selected = hosts_count();       /* stay on "add a host" */
            } else {
                return selected;
            }
        } else if ((pressed & PSP_CTRL_SQUARE) != 0 && selected < count) {
            edit_host(selected);
        } else if ((pressed & PSP_CTRL_TRIANGLE) != 0 && selected < count) {
            const host_entry *entry = hosts_at(selected);
            char question[HOST_NAME_LEN + 16];

            snprintf(question, sizeof(question), "delete \"%s\"?", entry->name);
            if (ui_confirm(question, "retyping it on the keyboard is the cost")
                    && hosts_remove(selected) != 0) {
                ui_message("it could not be removed", hosts_error(), GFX_RED);
            }
        } else if ((pressed & PSP_CTRL_SELECT) != 0 && selected < count) {
            /* Forgetting a host key deliberately, which servers that are
             * genuinely rebuilt need.
             *
             * It lives here rather than on the screen that refuses a changed
             * key, and that is the whole design: dismissing the alarm means
             * leaving the failed connection, finding the host, and confirming
             * something that says what it means. An "accept anyway" button on
             * the warning itself would be pressed before it was read. */
            const host_entry *entry = hosts_at(selected);
            char question[HOST_NAME_LEN + 32];

            if (knownhosts_lookup(entry->address, entry->port) == NULL) {
                ui_message("nothing is remembered for that host",
                           "its key will be trusted on first sight", GFX_GREY);
                continue;
            }
            snprintf(question, sizeof(question), "forget the key for \"%s\"?",
                     entry->name);
            if (ui_confirm(question,
                           "only if you know why it changed — this is the check")
                    && knownhosts_forget(entry->address, entry->port) != 0) {
                ui_message("it could not be forgotten", knownhosts_error(),
                           GFX_RED);
            }
        } else if ((pressed & PSP_CTRL_CIRCLE) != 0) {
            return -1;
        }
    }
}
