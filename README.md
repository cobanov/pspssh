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

The toolchain is real and the SSH library is cross-compiled; nothing runs on a
PSP yet. [docs/RESEARCH.md](docs/RESEARCH.md) records what decided the stack and
is worth reading before contributing.

- [x] Toolchain verified: `pspdev/pspdev` builds, networking libraries present
- [x] Prior art and platform research
- [x] wolfSSL and wolfSSH cross-compiled for PSP, with the modern algorithms
      asserted at build time — `tools/build-toolchain.sh`
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
tools/build-toolchain.sh
```

That cross-compiles wolfSSL and wolfSSH and bakes them into a
`pspssh/toolchain` image. It takes a while — the pspdev image is amd64 and runs
emulated on Apple Silicon — but it only has to happen once.

**The packaged wolfSSL is not usable here**, which is why the script builds its
own. `psp-pacman`'s build is configured for TLS and omits curve25519, Ed25519
and AES-CTR; wolfSSH links against it perfectly well and then cannot negotiate
with any current OpenSSH. The script asserts on both the resulting `options.h`
and the algorithm strings in `libwolfssh.a`, so a missing primitive fails the
build instead of the handshake.

## Licence

GPL-3.0-or-later, which follows from the stack: wolfSSH is GPLv3-or-commercial,
so anything depending on it is GPLv3.

wolfSSL is GPL-2.0-**or-later** — its `LICENSING` file and every source header
say "either version 2 of the License, or (at your option) any later version" —
so it upgrades to GPLv3 cleanly. The `GPL-2.0-only` in the PSP package metadata
is imprecise and worth correcting upstream.

[pspssh]: https://www.gamebrew.org/wiki/PSPSSH_PSP
[wolfssh]: https://github.com/wolfSSL/wolfssh
[wolfssl]: https://github.com/wolfSSL/wolfssl
