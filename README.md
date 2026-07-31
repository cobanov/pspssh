<p align="center">
  <img src="assets/thumbnail.webp" alt="pspssh" width="560">
</p>

<p align="center">
  An SSH client for the PlayStation Portable that can actually talk to a
  current OpenSSH server.
</p>

<p align="center">
  <a href="https://github.com/cobanov/pspssh/releases/latest"><img alt="release" src="https://img.shields.io/github/v/release/cobanov/pspssh?color=004aad&labelColor=1a1a1a"></a>
  <img alt="tests" src="https://img.shields.io/badge/tests-122%20offline%20%2B%206%20live-004aad?labelColor=1a1a1a">
  <img alt="hardware" src="https://img.shields.io/badge/PSP%20Go-confirmed-004aad?labelColor=1a1a1a">
  <a href="LICENSE"><img alt="licence" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-004aad?labelColor=1a1a1a"></a>
</p>

---

The PSP's existing SSH client is a 2008 port of Dropbear that offers
`diffie-hellman-group1-sha1`, `ssh-rsa` and `aes128-cbc` — none of which a
current OpenSSH accepts, so it fails before the session starts. The fix is not
to weaken your servers. This speaks `curve25519-sha256`, `ssh-ed25519` and
`aes256-ctr`, one algorithm per slot, so a weak session cannot be negotiated at
all.

Add your servers on the console, pick one, get a shell.

```
 pspssh                                     v2.0.0  cobanov.dev

   pve                     bb@192.168.8.45:2222
 > macmini                 mert@192.168.1.20:22
   laptop                  mert@10.0.0.4:22

   +  add a host

 X connect  [] edit  /\ delete  SEL forget key  O quit
```

- **Everything on the device.** Add, edit and delete hosts with the on-screen
  keyboard. No memory card shuffling.
- **Host keys are remembered.** Trust on first use; a changed key is refused,
  not warned about.
- **An 80-column terminal** with colour, cursor addressing and scroll regions.
- **Passwords are optional** — leave one out and it is asked for at connect
  time, then wiped.

Confirmed on a **PSP Go (N1000)** on custom firmware. Not yet implemented:
public key authentication.

## Install

Download `EBOOT.PBP` from [the latest release][latest] and copy it to the
memory card as:

```
PSP/GAME/pspssh/EBOOT.PBP
```

On a PSP Go the internal storage is `ef0:` rather than `ms0:`. Launch it from
the XMB under **Game**.

**Wi-Fi first.** This picks a connection you have already saved under
*Settings › Network Settings*; it does not join a network itself. A PSP does WEP
and WPA-TKIP — WPA2-AES needs the `wpa2psp` plugin on 6.61 PRO, and is already
there on ARK.

Leave a host's profile at `0` and it tries each saved connection until one
works. Worth doing: deleting a connection does not renumber the rest, so "the
only one left" is not reliably number 1.

Your servers live in `pspssh.hosts` beside the binary and remembered keys in
`pspssh.known`. Both survive an upgrade. A `pspssh.cfg` from an older version is
imported once and then left alone.

> **A stored password is plain text on a memory card.** Anyone holding the
> console can read it. Use an account you are willing to have on a games
> machine, or leave the password empty and type it each time.

## Use

**The list**

| | |
|---|---|
| **X** | connect |
| **□** | edit |
| **△** | delete |
| **SELECT** | forget this server's remembered key |
| **○** | quit |

A host you do not name takes the first label of its address, so
`pve.internal.example.org` becomes `pve`.

**The terminal**

| | |
|---|---|
| **X** | type a line |
| **□** | Enter |
| **△** | Ctrl-C |
| **○** | leave the session |
| **d-pad** | arrow keys — so shell history works |
| **SELECT** / **START** | Ctrl-D / Tab |
| **L** / **R** | Escape / Ctrl-L |

Typing is **line at a time**: the on-screen keyboard is a modal dialog, so you
compose a line and it is sent when you confirm. That suits `sh`, `ls` and `git`.
It does not suit `vim`, and nothing in this client can change that — the console
has no way to deliver a keystroke as it happens.

The title bar flashes red when the far side rings the bell.

**The first connection to a server**

You are shown its fingerprint and asked whether to trust it, exactly as `ssh`
does. Compare it against the server if you can:

```sh
ssh-keyscan -t ed25519 your.server | ssh-keygen -lf -
```

After that the same key must come back every time. If it does not, the
connection is refused and there is no button to override it — that screen is the
only moment an interception is visible. If you genuinely rebuilt the server,
select it in the list and press **SELECT** to forget the old key.

## Build

Needs Docker or OrbStack; no local toolchain.

```sh
tools/build-toolchain.sh     # once: cross-compiles wolfSSL and wolfSSH
tools/build-psp.sh           # -> build/psp/EBOOT.PBP
tools/test-offline.sh        # parsers and storage, no hardware needed
tools/test-host.sh           # the session, against a real OpenSSH
```

The toolchain step takes a while — the pspdev image is amd64 and runs emulated
on Apple Silicon — but only happens once. It builds wolfSSL from source because
the packaged one is configured for TLS and omits curve25519, Ed25519 and
AES-CTR; wolfSSH links against it happily and then cannot negotiate with
anything. The script asserts on the result, so a missing primitive fails the
build rather than the handshake.

Artwork in `assets/` is packed into the `EBOOT.PBP` when present — `ICON0.PNG`
(144×80), `PIC1.PNG` (480×272), and the rest of the XMB slots. See
`src/psp/Makefile`.

## How it is built

**A maintained SSH library, not a reimplementation.** [wolfSSH][wolfssh] over
[wolfSSL][wolfssl]. Vendoring a whole SSH program is what froze the 2008 port's
cryptography at the moment it was made.

**A portable core and a thin front end.** `src/core` has no PSP headers and
compiles for the host, so the protocol is exercised against a real OpenSSH from
a laptop — a key exchange either convinces OpenSSH or it does not, and a games
console with no debugger is a bad place to find out.

The same applies inside the front end: `hosts.c`, `knownhosts.c`, `term.c` and
`console.c` have no hardware in them, so **122 assertions run on a laptop**
covering the storage layer, the host key check, the escape-sequence parser and
the boot log. What genuinely needs a PSP is a screen, a radio and a keyboard.

[docs/RESEARCH.md](docs/RESEARCH.md) records what decided the stack.

## Licence

GPL-3.0-or-later, which follows from wolfSSH being GPLv3-or-commercial.

Screen font is [Spleen](https://github.com/fcambus/spleen) 6×12 by Frédéric
Cambus, BSD-2-Clause.

[latest]: https://github.com/cobanov/pspssh/releases/latest
[wolfssh]: https://github.com/wolfSSL/wolfssh
[wolfssl]: https://github.com/wolfSSL/wolfssl
