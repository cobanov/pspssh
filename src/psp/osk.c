/* pspssh — the on-screen keyboard.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See osk.h. The contract below is the SDK's, not ours, and the fixed values
 * come from the sample in $PSPDEV/psp/sdk/samples/utility/osk — guessing at
 * them produces a dialog that either never appears or never closes.
 */

#include "osk.h"
#include "gfx.h"
#include "pad.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <psputility.h>

#include <string.h>

/* Long enough for a password or a shell command line. The panel itself is
 * limited to what fits, and outtextlimit keeps the two in step. */
#define OSK_MAX 256

/* All OSK text is UTF-16. These live at file scope rather than on the stack
 * because the dialog reads them on its own threads for as long as it is up, and
 * a buffer that went out of scope would be read anyway. */
static unsigned short desc16[OSK_MAX];
static unsigned short in16[OSK_MAX];
static unsigned short out16[OSK_MAX];

static void to_utf16(const char *from, unsigned short *to, int max)
{
    int i = 0;

    if (from != NULL) {
        while (from[i] != '\0' && i < max - 1) {
            to[i] = (unsigned short)(unsigned char)from[i];
            i++;
        }
    }
    to[i] = 0;
}

/* Back to bytes, keeping only what this application can draw and store.
 *
 * The symbol panel offers en-dashes and smart quotes, and the font is ASCII —
 * so anything above 0x7e would be drawn as a blank, written into a
 * tab-separated file it might break, or sent to a shell that would see mojibake.
 * Replacing it with '?' is visible and harmless; dropping it silently would let
 * someone type a password they cannot retype. */
static void from_utf16(const unsigned short *from, char *to, int max)
{
    int i = 0;

    while (from[i] != 0 && i < max - 1) {
        unsigned short c = from[i];

        to[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
        i++;
    }
    to[i] = '\0';
}

static int input_type_for(osk_kind kind)
{
    switch (kind) {
    case OSK_DIGITS:
        return PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT;
    case OSK_HOSTNAME:
        /* The URL panel puts '.', '-' and '/' on the first page, which is most
         * of what a hostname is made of. */
        return PSP_UTILITY_OSK_INPUTTYPE_URL
               | PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT
               | PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE;
    case OSK_TEXT:
    default:
        return PSP_UTILITY_OSK_INPUTTYPE_ALL;
    }
}

osk_result osk_prompt(const char *prompt, const char *initial,
                      char *out, int out_len, osk_kind kind,
                      osk_backdrop_fn backdrop, void *backdrop_ctx)
{
    SceUtilityOskData data;
    SceUtilityOskParams params;
    int limit;
    int finished = 0;
    int appeared = 0;
    int waiting = 0;
    osk_result result = OSK_CANCELLED;

    if (out == NULL || out_len < 2) {
        return OSK_UNAVAILABLE;
    }

    limit = out_len - 1;
    if (limit > OSK_MAX - 1) {
        limit = OSK_MAX - 1;
    }

    to_utf16(prompt, desc16, OSK_MAX);
    to_utf16(initial, in16, OSK_MAX);
    memset(out16, 0, sizeof(out16));

    memset(&data, 0, sizeof(data));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = input_type_for(kind);
    data.desc = desc16;
    data.intext = in16;
    data.outtextlength = OSK_MAX;
    data.outtextlimit = limit;
    data.outtext = out16;

    memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    /* Asked of the system rather than assumed. buttonSwap in particular: X and
     * O are reversed between Japanese and Western consoles, and getting it
     * wrong swaps "confirm" with "cancel" — which would look like the keyboard
     * discarding everything the user typed. */
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                                &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP,
                                &params.base.buttonSwap);
    /* Fixed by the SDK. The dialog runs on its own threads and these are the
     * priorities it expects to be given. */
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

    if (sceUtilityOskInitStart(&params) < 0) {
        return OSK_UNAVAILABLE;
    }

    /* Frames spent waiting for the dialog to appear. Bounded, because looping
     * on a blank screen forever is the one outcome worse than saying no. */
    while (!finished) {
        int status;

        /* Through the graphics engine, not by writing pixels — see gfx.h. The
         * keyboard draws through the GE and queues its work asynchronously, so
         * a clear done by the processor races it and wins about as often as it
         * loses. That race is what a black screen instead of a keyboard looks
         * like. */
        gfx_gu_clear(GFX_BLACK);
        if (backdrop != NULL) {
            /* After the sync inside gfx_gu_clear, so the engine is idle and
             * these writes cannot be overtaken by the clear they follow. */
            backdrop(backdrop_ctx);
        }

        status = sceUtilityOskGetStatus();
        switch (status) {
        case PSP_UTILITY_OSK_DIALOG_VISIBLE:
            appeared = 1;
            sceUtilityOskUpdate(1);
            break;

        case PSP_UTILITY_OSK_DIALOG_QUIT:
            sceUtilityOskShutdownStart();
            break;

        case PSP_UTILITY_OSK_DIALOG_FINISHED:
            finished = 1;
            break;

        case PSP_UTILITY_OSK_DIALOG_NONE:
            /* "No dialog is active" is true before it has finished starting as
             * well as after it has shut down, and treating the two the same
             * ended this loop on its first frame — reporting a cancellation the
             * user never made, having cleared the screen to black on the way
             * out. Only a dialog that was once up can have come down. */
            if (appeared) {
                finished = 1;
            }
            break;

        default:
            /* INITING and INITED: keep drawing and wait. */
            break;
        }

        gfx_flip();

        if (!appeared && ++waiting > 300) {
            /* Five seconds. Something is wrong and the person holding the
             * console should be told rather than left looking at a blank
             * screen with no way back. */
            sceUtilityOskShutdownStart();
            while (sceUtilityOskGetStatus() != PSP_UTILITY_OSK_DIALOG_NONE
                    && waiting++ < 600) {
                gfx_gu_clear(GFX_BLACK);
                sceUtilityOskUpdate(1);
                gfx_flip();
            }
            return OSK_UNAVAILABLE;
        }

        /* The system asked us to close while a modal dialog was up. Tearing it
         * down first is not optional: leaving the OSK running and exiting
         * underneath it hangs the console rather than returning to the XMB. */
        if (pad_exit_requested()
                && sceUtilityOskGetStatus() == PSP_UTILITY_OSK_DIALOG_VISIBLE) {
            sceUtilityOskShutdownStart();
        }
    }

    if (data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) {
        return OSK_CANCELLED;
    }

    /* CHANGED and UNCHANGED both mean "the user pressed confirm". UNCHANGED
     * only says they did not alter what was already there, and treating that as
     * a cancel would make confirming an unedited field wipe it. */
    from_utf16(out16, out, out_len);
    result = OSK_ENTERED;

    /* The panel keeps whatever was typed until the next prompt overwrites it.
     * A password is the common case here, so it does not get to linger in a
     * static buffer for the rest of the session. */
    memset(out16, 0, sizeof(out16));
    memset(in16, 0, sizeof(in16));

    return result;
}
