# pspssh

An SSH client for the PlayStation Portable that can actually talk to a current
OpenSSH server.

There is already an SSH client for the PSP — [PSPSSH][pspssh], a 2008 port of
Dropbear 0.48. It cannot connect to anything modern: it offers
`diffie-hellman-group1-sha1`, `ssh-rsa` and `aes128-cbc`, and a current OpenSSH
offers none of those by default, so negotiation fails before the session starts.

The fix is not to weaken every server you own. It is a client that speaks
`curve25519-sha256`, `ssh-ed25519` and an authenticated cipher a 2026 server
still offers.

Target hardware is a **PSP Go (N1000)** on custom firmware. 480×272 gives an
**80×30** terminal, which is a proper one.

## Status

Nothing is built yet. The research is done and recorded in
[docs/RESEARCH.md](docs/RESEARCH.md) — it decides the stack and is worth reading
before contributing.

- [x] Toolchain verified: `pspdev/pspdev` builds, networking libraries present
- [x] Prior art and platform research
- [ ] Spike: does wolfSSH build for PSP, and is the licence combination sound
- [ ] Probe: measure input, screen and network on real hardware
- [ ] Transport working against a real OpenSSH server, from the host
- [ ] Terminal and input on the PSP
- [ ] A session

## How it will be built

**Stand on a maintained SSH library, do not reimplement one.** The plan is
[wolfSSH][wolfssh] over [wolfSSL][wolfssl], which is already packaged for the
PSP. wolfSSH supports `curve25519-sha256`, `ssh-ed25519`, `aes256-ctr` and
`hmac-sha2-256` — a fully modern session with nothing weakened. It lacks
`chacha20-poly1305@openssh.com`, which costs nothing, because a current OpenSSH
still offers AES-CTR by default and the PSP runs native code on a 333 MHz MIPS.

This is deliberately the opposite of what PSPSSH did. Vendoring a whole SSH
program froze its cryptography at the moment of the port; depending on a
maintained library means the algorithms can move forward.

**Portable core, thin PSP front end.** The SSH and terminal logic will have no
PSP headers in it, so it compiles for the host as well. That makes it testable
on a laptop against a real OpenSSH server — encodings can be proved with
vectors, a protocol cannot.

**Measure before designing.** The largest unknown is input: the PSP Go has no
keyboard, no IR port (Sony removed it after the 2000) and no USB host. The
system on-screen keyboard works but is slow, and whether a Bluetooth HID
keyboard can be paired is unverified. A probe answers that before anything is
designed around it.

## Building

Needs Docker or OrbStack; no local toolchain.

```sh
docker pull pspdev/pspdev
```

Build instructions will land with the first buildable target.

## Licence

GPL-3.0-or-later, which follows from the stack: wolfSSH is GPLv3-or-commercial,
so anything depending on it is GPLv3.

One item is still open and is tracked as an issue: the PSP wolfSSL package
declares `GPL-2.0-only`, which does not combine with GPLv3. wolfSSL Inc. ships
both products and intends them to be used together, so this is very probably
imprecise packaging metadata rather than a real conflict — but it needs reading
rather than assuming.

[pspssh]: https://www.gamebrew.org/wiki/PSPSSH_PSP
[wolfssh]: https://github.com/wolfSSL/wolfssh
[wolfssl]: https://github.com/wolfSSL/wolfssl
