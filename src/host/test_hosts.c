/* pspssh — the host list, tested off the device.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * src/psp/hosts.c is the one PSP module whose logic is decidable without a PSP:
 * it parses, validates and writes a file, and none of that needs a screen or a
 * radio. So it is compiled here against a POSIX shim for the six sceIo calls
 * and driven directly.
 *
 * The failure this exists to catch is a bad one to catch on hardware: a save
 * that loses every server somebody added, or a parser that reads a truncated
 * file as a shorter list and then writes that back.
 */

#include "../psp/hosts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (f != NULL) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

static char *read_file(const char *path)
{
    static char buffer[65536];
    FILE *f = fopen(path, "rb");
    size_t n = 0;

    buffer[0] = '\0';
    if (f != NULL) {
        n = fread(buffer, 1, sizeof(buffer) - 1, f);
        fclose(f);
    }
    buffer[n] = '\0';
    return buffer;
}

static void clean(void)
{
    remove("pspssh.hosts");
    remove("pspssh.hosts.tmp");
    remove("pspssh.cfg");
}

static host_entry sample(const char *name, const char *address,
                         const char *user, int port)
{
    host_entry entry;

    hosts_blank(&entry);
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    snprintf(entry.address, sizeof(entry.address), "%s", address);
    snprintf(entry.user, sizeof(entry.user), "%s", user);
    entry.port = port;
    return entry;
}

int main(void)
{
    printf("==> the host list, without a psp\n");

    /* --- an empty start ------------------------------------------------ */
    clean();
    check("an absent file is an empty list, not an error",
          hosts_load() == 0 && hosts_count() == 0);

    /* --- add, save, reload --------------------------------------------- */
    {
        host_entry a = sample("pve", "192.168.8.45", "bb", 2222);
        host_entry b = sample("laptop", "10.0.0.4", "mert", 22);

        snprintf(a.password, sizeof(a.password), "secret");
        b.profile = 2;

        check("a host can be added", hosts_add(&a) == 0);
        check("and another", hosts_add(&b) == 1);

        hosts_load();
        check("both come back after a reload", hosts_count() == 2);
        check("the first round-trips intact",
              hosts_at(0) != NULL
                  && strcmp(hosts_at(0)->name, "pve") == 0
                  && strcmp(hosts_at(0)->address, "192.168.8.45") == 0
                  && strcmp(hosts_at(0)->user, "bb") == 0
                  && strcmp(hosts_at(0)->password, "secret") == 0
                  && hosts_at(0)->port == 2222);
        check("a profile round-trips",
              hosts_at(1) != NULL && hosts_at(1)->profile == 2);
        check("an unset password stays unset",
              hosts_at(1)->password[0] == '\0');
    }

    /* --- edit and delete ------------------------------------------------ */
    {
        host_entry changed = *hosts_at(0);

        snprintf(changed.user, sizeof(changed.user), "root");
        check("a host can be replaced", hosts_replace(0, &changed) == 0);
        hosts_load();
        check("the replacement survives a reload",
              strcmp(hosts_at(0)->user, "root") == 0);

        check("a host can be removed", hosts_remove(0) == 0);
        hosts_load();
        check("the removal survives a reload, and takes the right one",
              hosts_count() == 1 && strcmp(hosts_at(0)->name, "laptop") == 0);

        check("removing an index that is not there is refused",
              hosts_remove(7) != 0);
    }

    /* --- what must not be storable -------------------------------------- */
    {
        host_entry bad = sample("x", "example.org", "me", 22);
        host_entry probe;

        probe = sample("x", "", "me", 22);
        check("an address is required", hosts_problem(&probe) != NULL);
        probe = sample("x", "example.org", "", 22);
        check("a user is required", hosts_problem(&probe) != NULL);
        probe = sample("x", "example.org", "me", 0);
        check("port 0 is refused", hosts_problem(&probe) != NULL);
        probe = sample("x", "example.org", "me", 65536);
        check("port 65536 is refused", hosts_problem(&probe) != NULL);

        /* A tab in a field would read back as a field boundary, so an entry
         * carrying one would come back as a different host — or as two. The
         * keyboard cannot produce a tab, but a file edited on a computer can. */
        snprintf(bad.user, sizeof(bad.user), "me\troot");
        check("a tab in a field is refused", hosts_problem(&bad) != NULL);

        snprintf(bad.user, sizeof(bad.user), "me");
        snprintf(bad.name, sizeof(bad.name), "one\ntwo");
        check("a newline in a field is refused", hosts_problem(&bad) != NULL);
    }

    /* --- a file that cannot be read in full ----------------------------- */
    {
        /* The old config reader took the first 1023 bytes and parsed whatever
         * landed, so a longer file lost its tail mid-line. Refusing the whole
         * file is the point: half a list read as a complete one would be
         * written back, and the missing half would be gone for good. */
        FILE *f = fopen("pspssh.hosts", "wb");
        int i;

        for (i = 0; i < 2000; i++) {
            fprintf(f, "host%d\t%d.example.org\t22\tme\t\t0\n", i, i);
        }
        fclose(f);

        check("a file too large to read whole is refused, not truncated",
              hosts_load() != 0);
        check("and nothing is loaded from it", hosts_count() == 0);
        check("and it says why", strstr(hosts_error(), "larger") != NULL);
    }

    /* --- malformed lines ------------------------------------------------ */
    {
        clean();
        write_file("pspssh.hosts",
                   "# a comment\n"
                   "\n"
                   "short\tline\n"
                   "good\texample.org\t22\tme\t\t0\n"
                   "bad\t\t22\tme\t\t0\n"          /* no address */
                   "alsogood\t10.0.0.1\t22\troot\tpw\t3\n");
        hosts_load();
        check("comments, blanks and short lines are skipped",
              hosts_count() == 2);
        check("the good ones are the ones kept",
              strcmp(hosts_at(0)->name, "good") == 0
                  && strcmp(hosts_at(1)->name, "alsogood") == 0);
    }

    /* --- the old single-server config ----------------------------------- */
    {
        clean();
        write_file("pspssh.cfg",
                   "# pspssh\n"
                   "host=192.168.1.10\n"
                   "port=2222\n"
                   "user=me\n"
                   "password=hunter2\n"
                   "profile=2\n");
        hosts_load();
        check("an old pspssh.cfg is imported", hosts_count() == 1);
        check("with everything it said",
              strcmp(hosts_at(0)->address, "192.168.1.10") == 0
                  && hosts_at(0)->port == 2222
                  && strcmp(hosts_at(0)->user, "me") == 0
                  && strcmp(hosts_at(0)->password, "hunter2") == 0
                  && hosts_at(0)->profile == 2);
        check("and it is written to the new file",
              strstr(read_file("pspssh.hosts"), "192.168.1.10") != NULL);
        check("the old file is left alone — somebody put it there by hand",
              access("pspssh.cfg", F_OK) == 0);

        hosts_load();
        check("importing does not happen twice", hosts_count() == 1);
    }

    /* --- an interrupted save -------------------------------------------- */
    {
        clean();
        /* The one moment a save is not atomic is between removing the old file
         * and renaming the new one into place. If the console dies there, the
         * list exists only under its temporary name — and starting empty would
         * throw away every server rather than the one being added. */
        write_file("pspssh.hosts.tmp",
                   "rescued\t10.0.0.9\t22\tme\t\t0\n");
        hosts_load();
        check("a save interrupted mid-rename is recovered",
              hosts_count() == 1 && strcmp(hosts_at(0)->name, "rescued") == 0);
        check("and the temporary file is put back where it belongs",
              access("pspssh.hosts", F_OK) == 0
                  && access("pspssh.hosts.tmp", F_OK) != 0);
    }

    /* --- passwords do not outlive the session --------------------------- */
    {
        /* clean() removes the files; the list in memory is only resynced by a
         * load. Forgetting that appends to whatever the previous block left
         * behind, which is how this test first "failed". */
        clean();
        hosts_load();
        {
            host_entry a = sample("s", "example.org", "me", 22);

            snprintf(a.password, sizeof(a.password), "topsecret");
            hosts_add(&a);
        }
        check("a password is there while it is needed",
              strcmp(hosts_at(0)->password, "topsecret") == 0);
        hosts_forget_passwords();
        check("and gone when it is not",
              hosts_at(0)->password[0] == '\0');
        check("without disturbing anything else",
              strcmp(hosts_at(0)->address, "example.org") == 0);
    }

    /* --- the list has a ceiling ----------------------------------------- */
    {
        int i;
        int refused = 0;

        clean();
        hosts_load();
        for (i = 0; i < HOSTS_MAX + 4; i++) {
            host_entry a = sample("h", "example.org", "me", 22);

            snprintf(a.name, sizeof(a.name), "h%d", i);
            if (hosts_add(&a) < 0) {
                refused++;
            }
        }
        check("the list stops at its ceiling rather than growing a file",
              hosts_count() == HOSTS_MAX && refused == 4);
        check("and says so", strstr(hosts_error(), "no room") != NULL);
    }

    clean();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
