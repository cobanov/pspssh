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

Target hardware is a **PSP Go (N1000)** on custom firmware, and it has been
confirmed on one.

## What it does

Add your servers on the device, pick one, get a shell.

```
 pspssh                                    v1.1.0  Mert Cobanov

   pve                  bb@192.168.8.45:2222
 > macmini              mert@192.168.1.20:22
   laptop               mert@10.0.0.4:22

   +  add a host

 X connect  [] edit  /\ delete  SEL forget key  O quit
```

- **Modern cryptography, and only that.** One algorithm per slot:
  `curve25519-sha256`, `ssh-ed25519`, `aes256-ctr`, `hmac-sha2-256`. A weak
  session cannot be negotiated because there is nothing weak to negotiate.
- **Servers managed on the console.** Add, edit and delete hosts with the
  system on-screen keyboard. No memory card shuffling.
- **Host keys are remembered.** Trust on first use, and a changed key is
  refused rather than warned about.
- **A real terminal, 80 columns wide.** Colours, cursor addressing, erase and
  scroll regions — what a shell sends, drawn as a shell means it, at the width
  it was formatted for.
- **Passwords are optional.** Leave one out and it is asked for at connect
  time, then forgotten.

## Status

**It works on hardware.** A PSP Go on custom firmware brings up Wi-Fi, connects,
completes a `curve25519-sha256` key exchange against a real OpenSSH server,
verifies its `ssh-ed25519` host key, authenticates, and runs a shell.
[docs/RESEARCH.md](docs/RESEARCH.md) records what decided the stack and is worth
reading before contributing.

- [x] wolfSSL and wolfSSH cross-compiled for the PSP with the modern algorithms
      asserted at build time — `tools/build-toolchain.sh`
- [x] A full session against a real OpenSSH from the host — `tools/test-host.sh`
- [x] An installable `EBOOT.PBP` — `tools/build-psp.sh`
- [x] **Confirmed on a PSP Go**: authenticated, shell opened, output returned
- [x] Our own screen, so there can be a keyboard and a terminal
- [x] Hosts added, edited and deleted on the device
- [x] Typing, with the on-screen keyboard
- [x] Host keys remembered, and a changed one refused
- [ ] Public key authentication
- [ ] Whether a Bluetooth keyboard can be paired
      ([#2](https://github.com/cobanov/pspssh/issues/2))

## Using it

### The list

| | |
|---|---|
| **X** | connect |
| **□** | edit |
| **△** | delete |
| **SELECT** | forget this server's remembered key |
| **○** | quit |

### The terminal

| | |
|---|---|
| **X** | type a line |
| **□** | Enter |
| **△** | Ctrl-C |
| **○** | leave the session |
| **d-pad** | arrow keys — so shell history works |
| **SELECT** / **START** | Ctrl-D / Tab |
| **L** / **R** | Escape / Ctrl-L |

The on-screen keyboard is a modal dialog, so typing is **line at a time**: you
compose a line and it is sent when you confirm. That suits `sh`, `ls`, `git` and
anything else you would drive from a prompt. It does not suit `vim`, and no
amount of work on this client will change that — the console has no way to
deliver a keystroke as it happens.

### The first connection to a server

You are shown its fingerprint and asked whether to trust it, exactly as `ssh`
does. Compare it against the server if you can:

```sh
ssh-keyscan -t ed25519 your.server | ssh-keygen -lf -
```

After that, the same key must come back every time. If it does not, the
connection is refused and there is no button to override it — that screen is the
only moment an interception is visible.

If you genuinely rebuilt the server, select it in the list and press **SELECT**
to forget the old key. That is deliberately not on the warning screen: a button
next to an alarm gets pressed instead of read.

## The artwork

`assets/` holds what the XMB shows, packed into the `EBOOT.PBP` rather than
copied beside it. Each is picked up when present and the build works without
any of them.

| | | |
|---|---|---|
| `ICON0.PNG` | 144×80 | the icon in the game list |
| `ICON1.PMF` | 144×80 | an animation shown instead of the icon |
| `PIC0.PNG` | 480×272 | drawn over the background |
| `PIC1.PNG` | 480×272 | the background, while this is the highlighted item |
| `SND0.AT3` | ATRAC3 | music, while it is highlighted — see [#51](https://github.com/cobanov/pspssh/issues/51) |

There is no `SND0.AT3` here. Several conformant ones were tried and none of them
played; [#51](https://github.com/cobanov/pspssh/issues/51) records what was
established so the next attempt does not start from nothing. Drop a file in and
the build will pick it up.

## Building

Needs Docker or OrbStack; no local toolchain.

```sh
tools/build-toolchain.sh     # once: cross-compiles wolfSSL and wolfSSH
tools/build-psp.sh           # -> build/psp/EBOOT.PBP
tools/test-offline.sh        # the parsers and the storage, no hardware
tools/test-host.sh           # the session, against a real OpenSSH, from here
```

The toolchain build takes a while — the pspdev image is amd64 and runs emulated
on Apple Silicon — but it only has to happen once.

**The packaged wolfSSL is not usable here**, which is why the script builds its
own. `psp-pacman`'s build is configured for TLS and omits curve25519, Ed25519
and AES-CTR; wolfSSH links against it perfectly well and then cannot negotiate
with any current OpenSSH. The script asserts on the resulting `options.h`, on a
compile check, and on the exported symbols, so a missing primitive fails the
build instead of the handshake.

## Installing

Copy `build/psp` to the memory card as `PSP/GAME/pspssh/` — on a PSP Go the
internal storage is `ef0:` rather than `ms0:`. Launch it from the XMB under
Game, and add your servers from the list.

The Wi-Fi profile is one you have already saved under **Settings › Network
Settings**; this picks a saved connection, it does not join a network. Leave a
host's profile at 0 and it tries each one until something works — the numbers do
not renumber when you delete a connection, so "the only one left" is not
reliably number 1.

A PSP does WEP and WPA-TKIP. WPA2-AES needs the `wpa2psp` plugin on 6.61 PRO;
on ARK CFW it is already there.

### Where things are kept

| | |
|---|---|
| `pspssh.hosts` | your servers, one per line |
| `pspssh.known` | remembered host keys |
| `pspssh.cfg` | the old single-server config — imported once if present, then left alone |

**A stored password is plain text on a memory card.** Anyone holding the console
can read it, so either use an account you are willing to have on a games machine
or leave the password empty and type it each time. Public key authentication is
wanted and not written yet.

## How it is built

**Stand on a maintained SSH library, do not reimplement one.** [wolfSSH][wolfssh]
over [wolfSSL][wolfssl], built from source with the algorithms this needs.
Vendoring a whole SSH program is what froze PSPSSH's cryptography at the moment
of its port; depending on a maintained library means it can move forward.

**Portable core, thin PSP front end.** `src/core` has no PSP headers in it and
compiles for the host, so the protocol is exercised against a real OpenSSH from
a laptop. Encodings can be proved with test vectors; a key exchange either
convinces OpenSSH or it does not, and a games console with no debugger is a bad
place to find that out.

The same principle applies inside the front end. `hosts.c`, `knownhosts.c` and
`term.c` have no hardware in them either, so 106 assertions run on a laptop —
the storage layer, the host key check, and the escape-sequence parser. What is
left needing a PSP is a screen, a radio and a system keyboard.

## Licence

GPL-3.0-or-later, which follows from the stack: wolfSSH is GPLv3-or-commercial,
so anything depending on it is GPLv3.

wolfSSL is GPL-2.0-**or-later** — its `LICENSING` file and every source header
say "either version 2 of the License, or (at your option) any later version" —
so it upgrades to GPLv3 cleanly. The `GPL-2.0-only` in the PSP package metadata
is imprecise and worth correcting upstream.

The screen font is [Spleen](https://github.com/fcambus/spleen) 6×12 by Frédéric
Cambus, BSD-2-Clause, in `third_party/` with `tools/make-font.py` deriving the
header the build uses.

[pspssh]: https://www.gamebrew.org/wiki/PSPSSH_PSP
[wolfssh]: https://github.com/wolfSSL/wolfssh
[wolfssl]: https://github.com/wolfSSL/wolfssl
