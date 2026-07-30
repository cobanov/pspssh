# Research: what exists, what runs on a PSP, and what we should build on

Written before any implementation, because the central question — write an SSH
implementation or stand on one — has an evidence-based answer and guessing it
would cost weeks.

## 1. Prior art: PSPSSH, and the lesson it teaches

**PSPSSH** by Ludovic Jacomme (Zx-81), last released 1.2.0 in November 2008, is
an SSH2 client for the PSP. It is a port of **Dropbear 0.48.1**, with VT100
emulation taken from Danzel's PSP telnet client. It has multi-session support
and RSA key authentication.

Two things follow from it.

**It proves the shape works.** A full SSH implementation compiles and runs on a
PSP. That is not a question anyone needs to re-answer.

**It also shows the failure mode we must avoid.** Dropbear 0.48 is from 2007. It
offers `diffie-hellman-group1-sha1`, `ssh-rsa` and `aes128-cbc`. A current
OpenSSH offers none of those by default, so PSPSSH and a modern server have
nothing in common and the connection dies during negotiation. Porting a whole
SSH program froze its cryptography at the moment of the port.

That is the same situation BBSSH was in on BlackBerry, and it is the reason this
project exists. **Whatever we build must be able to follow the algorithms
forward**, which argues for depending on something maintained rather than
vendoring a snapshot.

A third detail is worth stealing: PSPSSH ships a **command and word list
editor**. Someone wrote a whole feature so users could store commands and recall
them, because typing on a PSP is painful enough to justify it. That is a
usability finding handed to us for free.

## 2. The PSP platform in 2026

The homebrew toolchain is in better shape than the hardware's age suggests.

**pspdev/pspsdk** is open source and actively maintained — there is a
`v20260601` SDK release, and a `psp-pacman` package manager. The Docker image
`pspdev/pspdev` gives a working `psp-gcc` with no local setup, which is
verified: it builds and the networking libraries are present
(`libpspnet_inet`, `libpspnet_apctl`, `libpspnet_resolver`, `libpspwlan`).

**Over 50 libraries are packaged**, including several that matter here:

| Package | Why it matters |
| --- | --- |
| `wolfssl` | 5.7.0, full modern crypto **including Ed25519** |
| `mbedtls` | 2.28.10 with a PSP patch — but see the Ed25519 problem below |
| `libintrafont` | PSP's own PGF fonts, variable width |
| `pspirkeyb` | IR keyboard support — irrelevant on a Go, see §5 |
| `sdl2`, `sdl2-ttf`, `curl`, `sqlite` | the rest of a normal toolbox |

**Network initialisation** is a fixed sequence, from the official PSPSDK
samples:

```c
sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
pspSdkInetInit();
sceNetApctlConnect(config);            // config = a saved Wi-Fi profile
while (state != 4) {                    // 4 == connected, IP assigned
    sceNetApctlGetState(&state);
    sceKernelDelayThread(50 * 1000);
}
sceNetApctlGetInfo(8, &info);           // the IP we got
```

After that it is BSD sockets: `socket()`, `connect()`, `select()`, `read()`,
`write()`. DNS is separate — `sceNetResolverCreate` with a 1024-byte buffer,
then `sceNetResolverStartNtoA`.

Note that the Wi-Fi profile is one the user configured in the PSP's own
settings. The app picks a profile index; it does not do the joining.

## 3. The crypto decision

This is the one that shapes everything.

**berryssh wrote its own cryptography for a reason that does not apply here.**
CLDC 1.1 has no `java.security`, no `javax.crypto`, no `BigInteger`, and no
library could be made to run — the platform genuinely offered nothing. On the
PSP, wolfSSL is one `psp-pacman` command away. Reimplementing here would be
choosing difficulty for its own sake.

### Candidates

| | curve25519-sha256 | ssh-ed25519 | chacha20-poly1305 | Shape | PSP status |
| --- | --- | --- | --- | --- | --- |
| **wolfSSH** + wolfSSL | yes | yes | **no** | library | wolfSSL packaged |
| libssh2 + mbedTLS | yes | **no** — needs an OpenSSL backend | yes | library | mbedTLS packaged |
| Dropbear (current) | yes | yes | yes | **program** | proven, but at 0.48 |
| Our own, over wolfSSL | our choice | our choice | our choice | — | — |

Read from the source rather than the documentation: wolfSSH's `src/internal.c`
names `curve25519-sha256`, `curve25519-sha256@libssh.org`, `ssh-ed25519`,
`aes256-ctr`, `hmac-sha2-256` and `hmac-sha2-512`. It does **not** contain
`chacha20-poly1305@openssh.com`.

### Two findings that decide it

**mbedTLS has no Ed25519.** It is a long-standing gap and it is not in 2.28 or
3.x. Since `ssh-ed25519` is the host key type every modern OpenSSH presents by
default, a mbedTLS-based stack cannot verify the host key we will actually be
shown. That removes the libssh2 + mbedTLS path unless we bolt on a separate
Ed25519 implementation.

**The missing ChaCha20 is not a blocker.** A current OpenSSH's default cipher
list is `chacha20-poly1305@openssh.com,aes128-ctr,aes192-ctr,aes256-ctr,
aes128-gcm@openssh.com,aes256-gcm@openssh.com`, and its default MACs include
`hmac-sha2-256`. So wolfSSH negotiates **curve25519-sha256 + ssh-ed25519 +
aes256-ctr + hmac-sha2-256** — entirely modern, and nothing on the server has to
be weakened.

berryssh preferred ChaCha20 because AES is slow in interpreted Java on a 2011
phone CPU. That argument does not carry: the PSP runs native code on a 333 MHz
MIPS, and a terminal moves a few kilobytes a second.

### Where this leaves us

wolfSSH is the strongest candidate: it is a **library** rather than a program,
it is written for embedded targets, and its crypto dependency is already
packaged for the PSP. Dropbear has better algorithm coverage but is
program-shaped — porting it means gutting a main loop that assumes fork, a
controlling terminal and POSIX signals, which is exactly the work PSPSSH did
once and then could never carry forward.

Open questions before committing, both recorded as issues:

- **Licensing.** wolfSSH is GPLv3-or-commercial. The PSP wolfSSL package
  declares `GPL-2.0-only`, and GPL-2.0-only does not combine with GPLv3.
  Upstream wolfSSL is dual-licensed and wolfSSL Inc. ships both products
  together, so this is very probably a packaging imprecision rather than a real
  conflict — but it must be read properly, not assumed.
- **Does wolfSSH build for PSP at all?** It is not in `psp-packages`. It is
  ANSI C targeting embedded systems, so it should, but "should" is not
  "does", and finding out is cheap.

## 4. Screen and terminal

480×272. With a 6×9 bitmap cell that is **80×30** — a standard-width terminal
with a few rows to spare, better than either phone this project's sibling
targets.

`libintrafont` renders the PSP's own PGF fonts, which are proportional. A
terminal wants a fixed grid, so the atlas approach is the right one here for the
same reason it was on the handsets: proportional fonts cannot make a character
grid, and the system font sizes are not chosen for one.

## 5. Input, and the part that is genuinely unsolved

The PSP has no keyboard. `sceUtilityOsk` provides the system on-screen keyboard;
it is ASCII-only, which is sufficient for a shell but slow to use.

`pspirkeyb` supports infrared keyboards, and PSPSSH used exactly that — its
1.0.6 release added IR keyboard support. **This does not help a PSP Go.** Sony
removed the IR port after the PSP-2000 (the 3000 has no diode at all) and the Go
has none; it has Bluetooth instead.

So on the target hardware the input options are:

1. `sceUtilityOsk` — works, slow
2. A stored command list, as PSPSSH did — mitigates the above
3. Bluetooth HID keyboard — **unverified.** The Go's Bluetooth is officially for
   headsets and tethering. Whether homebrew can pair an HID keyboard is the
   single largest unknown in this project, and it decides whether the result is
   a tool or a demonstration.
4. USB keyboard — **ruled out.** The Go's micro-USB is device mode, not host.

This is why the first deliverable is a probe rather than a client.

## Sources

- [PSPSSH — GameBrew](https://www.gamebrew.org/wiki/PSPSSH_PSP) ·
  [v1.0.6 IR keyboards](https://www.dcemu.co.uk/vbulletin/threads/64276-PSPSSH-SSH2-Client-for-PSP-v1-0-6-(IR-Keyboards-support))
- [pspdev/pspsdk](https://github.com/pspdev/pspsdk) ·
  [psp-packages](https://github.com/pspdev/psp-packages) ·
  [net sample](https://github.com/pspdev/pspsdk/blob/master/src/samples/net/simple/main.c)
- [wolfSSH](https://github.com/wolfSSL/wolfssh) ·
  [wolfSSH product page](https://www.wolfssl.com/products/wolfssh/)
- [Dropbear algorithm support](https://ssh-comparison.quendi.de/impls/dropbear.html) ·
  [size on MIPS](https://github.com/openwrt/openwrt/pull/2919/files)
- [intraFont](https://github.com/PSP-Archive/intraFont)
