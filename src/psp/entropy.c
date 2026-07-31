/* pspssh — where random numbers come from on a PSP.
 *
 * Copyright (C) 2026 Mert Cobanov
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wolfSSL gets its seed from somewhere platform-specific: /dev/urandom on a
 * desktop, a hardware peripheral on a microcontroller that has one. A PSP has
 * neither, so wc_InitRng returned RNG_FAILURE_E (-199) and every session died
 * before it began.
 *
 * The console offers no random source at all. What it does offer is a
 * microsecond clock and a machine with interrupts, and the gap between "how
 * long should this take" and "how long did it take" is not perfectly
 * predictable. That gap is harvested here.
 *
 * ## What this is, and what it is not
 *
 * This is a jitter-based entropy collector feeding a SHA-256 pool. It is not a
 * hardware random number generator and it should not be described as one. Each
 * timing sample contributes a small and unquantified number of bits, so the
 * pool takes many of them and hashes the lot.
 *
 * Two things keep it honest rather than merely plausible:
 *
 * It **counts distinct deltas** and refuses to produce output if the timer
 * looks stuck. A clock too coarse to jitter would yield a pool seeded with
 * nothing, and quietly handing back predictable bytes is far worse than
 * failing — predictable bytes here mean a guessable session key.
 *
 * It **ratchets**. After every block of output the pool is hashed forward, so
 * recovering the state later reveals nothing about what was already handed
 * out.
 */

#include <pspkernel.h>
#include <psprtc.h>

#include <string.h>

#include <wolfssl/wolfcrypt/sha256.h>

/* Enough samples that even a pessimistic estimate of a bit or two each clears
 * a 256-bit pool several times over. On this hardware the whole loop is a
 * fraction of a second, paid once per session. */
#define SAMPLES 512

/* If fewer than this many samples differ from the one before, the clock is not
 * giving us jitter and there is nothing to build a key on. */
#define MIN_DISTINCT 64

static void mix(wc_Sha256 *pool, const void *data, unsigned int len)
{
    wc_Sha256Update(pool, (const byte *)data, len);
}

/* A small amount of work whose duration the caller cannot predict exactly.
 * Volatile so the compiler cannot decide it is pointless and remove it, which
 * would remove the jitter with it. */
static void wobble(int rounds)
{
    volatile unsigned int acc = 0;
    int i;

    for (i = 0; i < rounds; i++) {
        acc += (unsigned int)i * 2654435761u;
        acc ^= acc >> 13;
    }
}

/*
 * Called by wolfSSL through CUSTOM_RAND_GENERATE_SEED. Returns 0 on success,
 * non-zero to tell it the seed could not be produced.
 */
int pspssh_generate_seed(unsigned char *output, unsigned int sz)
{
    wc_Sha256 pool;
    unsigned char block[WC_SHA256_DIGEST_SIZE];
    u64 previous = 0;
    u64 delta_previous = 0;
    int distinct = 0;
    unsigned int filled = 0;
    int i;

    if (output == NULL || sz == 0) {
        return -1;
    }
    if (wc_InitSha256(&pool) != 0) {
        return -1;
    }

    /* Start from things that differ between boots and between machines. None
     * of these is secret; they are here so two consoles starting in identical
     * conditions still diverge, not to provide entropy on their own. */
    {
        u64 now = sceKernelGetSystemTimeWide();
        u64 tick = 0;
        void *stack_address = &pool;
        SceUID thread = sceKernelGetThreadId();

        mix(&pool, &now, sizeof(now));
        if (sceRtcGetCurrentTick(&tick) == 0) {
            mix(&pool, &tick, sizeof(tick));
        }
        mix(&pool, &stack_address, sizeof(stack_address));
        mix(&pool, &thread, sizeof(thread));
    }

    /* The harvest. Each round does a little work, measures how long it took,
     * and folds the measurement in. The counted quantity is the *second*
     * difference — how much the duration changed from the previous round —
     * because a constant offset carries no information and would otherwise
     * flatter the health check. */
    for (i = 0; i < SAMPLES; i++) {
        u64 before = sceKernelGetSystemTimeWide();
        u64 after;
        u64 delta;

        wobble(64 + (i & 31));
        /* Yielding invites the scheduler and whatever else the console is
         * doing to interfere, which is the point. */
        sceKernelDelayThread(0);

        after = sceKernelGetSystemTimeWide();
        delta = after - before;

        mix(&pool, &after, sizeof(after));
        mix(&pool, &delta, sizeof(delta));

        if (i > 0 && delta != delta_previous) {
            distinct++;
        }
        delta_previous = delta;
        previous = after;
    }
    (void)previous;

    if (distinct < MIN_DISTINCT) {
        /* The clock is not jittering. Rather than hand back something that
         * looks random and is not, say no — a failed connection is recoverable
         * and a predictable session key is not. */
        wc_Sha256Free(&pool);
        return -1;
    }

    mix(&pool, &distinct, sizeof(distinct));

    /* Expand to the requested length, ratcheting as it goes: each block is the
     * hash of the pool, and the block is then folded back in so the next one
     * cannot be derived from it. */
    while (filled < sz) {
        wc_Sha256 copy;
        unsigned int take;

        if (wc_InitSha256(&copy) != 0) {
            wc_Sha256Free(&pool);
            return -1;
        }
        /* Hash the pool's current state without consuming it. */
        {
            u64 counter = (u64)filled;
            wc_Sha256Update(&copy, (const byte *)&counter, sizeof(counter));
        }
        wc_Sha256GetHash(&pool, block);
        wc_Sha256Update(&copy, block, sizeof(block));
        wc_Sha256Final(&copy, block);
        wc_Sha256Free(&copy);

        take = sz - filled;
        if (take > sizeof(block)) {
            take = sizeof(block);
        }
        memcpy(output + filled, block, take);
        filled += take;

        /* Ratchet. */
        mix(&pool, block, sizeof(block));
    }

    memset(block, 0, sizeof(block));
    wc_Sha256Free(&pool);
    return 0;
}
