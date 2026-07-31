/* pspssh — the remembered host keys, tested off the device.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is the module that decides whether the far side is who it was last time,
 * so it is the last one that should be taken on trust. Everything here is a way
 * the check could quietly stop checking: a file read short, a record loaded
 * half-way, an address that matched something it should not.
 */

#include "../psp/knownhosts.h"

#include <stdio.h>
#include <string.h>

#define FP_A "SHA256:9tqjakW/Ia6U4hT3VgAv8EXXCxC1d3ez9mr5qjVTRZs"
#define FP_B "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

static int passed;
static int failed;

static void check(const char *what, int ok)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

static void write_file(const char *text)
{
    FILE *f = fopen("pspssh.known", "wb");

    if (f != NULL) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

static void clean(void)
{
    remove("pspssh.known");
    remove("pspssh.known.tmp");
}

static int matches(const char *address, int port, const char *fingerprint)
{
    const char *known = knownhosts_lookup(address, port);

    return known != NULL && strcmp(known, fingerprint) == 0;
}

int main(void)
{
    printf("==> the remembered host keys, without a psp\n");

    clean();
    check("an absent file means nothing is known, and is not an error",
          knownhosts_load() == 0 && knownhosts_count() == 0);
    check("an unknown server is unknown",
          knownhosts_lookup("192.168.8.45", 22) == NULL);

    /* --- remembering ----------------------------------------------------- */
    check("a key can be remembered",
          knownhosts_remember("192.168.8.45", 2222, FP_A) == 0);
    check("and is found again", matches("192.168.8.45", 2222, FP_A));
    check("and survives a reload",
          knownhosts_load() == 0 && matches("192.168.8.45", 2222, FP_A));

    /* The port is part of the identity. Two sshd instances on one machine are
     * two servers with two keys, and treating them as one would mean the second
     * connection always looks like an attack. */
    check("a different port on the same address is a different server",
          knownhosts_lookup("192.168.8.45", 22) == NULL);

    /* --- the check itself -------------------------------------------------- */
    check("the same key matches", matches("192.168.8.45", 2222, FP_A));
    check("a different key does not",
          !matches("192.168.8.45", 2222, FP_B));

    /* Prefixes must not match. A fingerprint compared with anything less than
     * equality is a fingerprint that can be forged by truncation. */
    check("a prefix of the right key does not match",
          !matches("192.168.8.45", 2222, "SHA256:9tqjak"));

    /* --- replacing and forgetting ------------------------------------------ */
    check("remembering again replaces rather than duplicating",
          knownhosts_remember("192.168.8.45", 2222, FP_B) == 0
              && knownhosts_count() == 1
              && matches("192.168.8.45", 2222, FP_B));

    check("a key can be forgotten deliberately",
          knownhosts_forget("192.168.8.45", 2222) == 0
              && knownhosts_lookup("192.168.8.45", 2222) == NULL);
    check("and the forgetting survives a reload",
          knownhosts_load() == 0
              && knownhosts_lookup("192.168.8.45", 2222) == NULL);
    check("forgetting something already unknown is not a failure",
          knownhosts_forget("nothing.example.org", 22) == 0);

    /* --- several servers ---------------------------------------------------- */
    {
        clean();
        knownhosts_load();
        knownhosts_remember("a.example.org", 22, FP_A);
        knownhosts_remember("b.example.org", 22, FP_B);
        knownhosts_remember("c.example.org", 22, FP_A);
        knownhosts_load();
        check("several servers round-trip", knownhosts_count() == 3);
        check("and each keeps its own key",
              matches("a.example.org", 22, FP_A)
                  && matches("b.example.org", 22, FP_B)
                  && matches("c.example.org", 22, FP_A));

        knownhosts_forget("b.example.org", 22);
        knownhosts_load();
        check("forgetting one leaves the others",
              knownhosts_count() == 2
                  && matches("a.example.org", 22, FP_A)
                  && matches("c.example.org", 22, FP_A)
                  && knownhosts_lookup("b.example.org", 22) == NULL);
    }

    /* --- records that must not load ----------------------------------------- */
    {
        /* Each of these would produce an entry that never matches, so every
         * connection to that server would report a changed key. An alarm that
         * is always wrong is worse than no alarm: it teaches people to click
         * through the one that is right. */
        clean();
        write_file("# a comment\n"
                   "\n"
                   "noTabsHere\n"
                   "missing.fingerprint.example.org\t22\t\n"
                   "\t22\t" FP_A "\n"
                   "bad.port.example.org\tnotanumber\t" FP_A "\n"
                   "good.example.org\t22\t" FP_A "\n");
        knownhosts_load();
        check("comments, blanks and broken records are dropped",
              knownhosts_count() == 1);
        check("and the sound one is the one kept",
              matches("good.example.org", 22, FP_A));
    }

    {
        /* A field longer than the buffer would be cut to fit, and a truncated
         * address belongs to a different server than the one written down. */
        char line[400];
        int i;

        clean();
        for (i = 0; i < 200; i++) {
            line[i] = 'a';
        }
        line[200] = '\0';
        {
            FILE *f = fopen("pspssh.known", "wb");

            fprintf(f, "%s\t22\t%s\n", line, FP_A);
            fprintf(f, "fine.example.org\t22\t%s\n", FP_A);
            fclose(f);
        }
        knownhosts_load();
        check("an address too long to store is dropped, not truncated",
              knownhosts_count() == 1
                  && matches("fine.example.org", 22, FP_A));
    }

    /* --- a file that cannot be read whole ------------------------------------ */
    {
        FILE *f;
        int i;

        clean();
        f = fopen("pspssh.known", "wb");
        for (i = 0; i < 4000; i++) {
            fprintf(f, "h%d.example.org\t22\t%s\n", i, FP_A);
        }
        fclose(f);

        check("a file too large to read whole is refused",
              knownhosts_load() != 0);
        /* This is the dangerous one. Loading a partial list means the servers
         * in the missing part look new, and a server that looks new is trusted
         * on sight — the check turning itself off without saying so. */
        check("and nothing is loaded, so no server is silently trusted",
              knownhosts_count() == 0);
        check("and it says why", knownhosts_error() != NULL
              && strlen(knownhosts_error()) > 0);
    }

    /* --- what cannot be stored ------------------------------------------------ */
    {
        clean();
        knownhosts_load();
        check("an empty fingerprint is refused",
              knownhosts_remember("x.example.org", 22, "") != 0);
        check("an empty address is refused",
              knownhosts_remember("", 22, FP_A) != 0);
        /* A tab would read back as a field boundary, so a key remembered for
         * one address could return attached to another. */
        check("a tab in an address is refused",
              knownhosts_remember("a\tb", 22, FP_A) != 0);
        check("a newline in a fingerprint is refused",
              knownhosts_remember("x.example.org", 22, "SHA256:aa\nbb") != 0);
        check("and none of them were stored", knownhosts_count() == 0);
    }

    /* --- an interrupted save ---------------------------------------------------- */
    {
        clean();
        {
            FILE *f = fopen("pspssh.known.tmp", "wb");

            fprintf(f, "rescued.example.org\t22\t%s\n", FP_A);
            fclose(f);
        }
        knownhosts_load();
        check("a save interrupted mid-rename is recovered rather than lost",
              knownhosts_count() == 1
                  && matches("rescued.example.org", 22, FP_A));
    }

    clean();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
