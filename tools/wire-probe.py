#!/usr/bin/env python3
"""A listener that shows exactly what a client says, and when.

    tools/wire-probe.py <port>                  # answer as a stub server
    tools/wire-probe.py <port> <host>:<port>    # forward to a real one, logging

With a target it is a proxy: every byte in both directions is dumped with a
timestamp while the real server does the talking. That is the mode worth using
once a client gets past its own identification string, because it shows the
whole handshake rather than the first half of it.

Point the PSP at this instead of a real server and the whole conversation
becomes visible: whether it connects, whether it sends its identification
string, how long it takes to get there, and what it does with the reply.

This exists because the project's test sshd runs in a container, and Docker's
NAT rewrites every client address to the bridge — so the server log cannot tell
a PSP from a laptop, and "the connection closed before identification" could
have been either. Listening directly on the host removes the middleman.

It speaks just enough SSH to keep a client moving: it sends a plausible
identification string, then reads whatever comes next. That is enough to place
a fault on one side of the version exchange or the other, which is the question
worth answering first.
"""
import binascii
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2223
TARGET = None
if len(sys.argv) > 2:
    host, _, port = sys.argv[2].rpartition(":")
    TARGET = (host, int(port))
BANNER = b"SSH-2.0-OpenSSH_9.2p1 Debian-2+deb12u10\r\n"


def show(direction, data, started):
    at = time.monotonic() - started
    printable = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in data)
    print(f"  [{at:7.3f}s] {direction} {len(data):5d} bytes")
    for i in range(0, min(len(data), 128), 32):
        chunk = data[i:i + 32]
        print(f"            {binascii.hexlify(chunk).decode()}")
        print(f"            {printable[i:i + 32]}")
    if len(data) > 128:
        print(f"            ... {len(data) - 128} more")
    sys.stdout.flush()


def proxy(conn, peer):
    """Forward to the real server, showing both sides as it goes."""
    started = time.monotonic()
    print(f"\n=== connection from {peer[0]}:{peer[1]} -> {TARGET[0]}:{TARGET[1]}")
    sys.stdout.flush()

    try:
        upstream = socket.create_connection(TARGET, timeout=10)
    except Exception as e:
        print(f"  cannot reach {TARGET[0]}:{TARGET[1]} — {e}")
        conn.close()
        return

    done = threading.Event()

    def pump(src, dst, arrow):
        try:
            while not done.is_set():
                data = src.recv(4096)
                if not data:
                    break
                show(arrow, data, started)
                dst.sendall(data)
        except Exception:
            pass
        finally:
            done.set()

    a = threading.Thread(target=pump, args=(conn, upstream, "client -->"), daemon=True)
    b = threading.Thread(target=pump, args=(upstream, conn, "server <--"), daemon=True)
    a.start()
    b.start()
    done.wait()
    time.sleep(0.2)
    print(f"  [{time.monotonic() - started:7.3f}s] finished")
    sys.stdout.flush()
    for s_ in (conn, upstream):
        try:
            s_.close()
        except Exception:
            pass


def serve(conn, peer):
    started = time.monotonic()
    print(f"\n=== connection from {peer[0]}:{peer[1]}")
    sys.stdout.flush()

    try:
        # Sent immediately, as a real server does. A client that never replies
        # to this is stuck before or during its own version exchange, which is
        # a very different fault from one that fails during key exchange.
        conn.sendall(BANNER)
        show("-->", BANNER, started)

        conn.settimeout(60)
        total = 0
        while True:
            data = conn.recv(4096)
            if not data:
                print(f"  [{time.monotonic() - started:7.3f}s] client closed "
                      f"after sending {total} bytes")
                break
            total += len(data)
            show("<--", data, started)
    except socket.timeout:
        print(f"  [{time.monotonic() - started:7.3f}s] nothing further for 60s "
              f"— the client is stuck, not disconnected")
    except Exception as e:
        print(f"  error: {e}")
    finally:
        conn.close()
        sys.stdout.flush()


def main():
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", PORT))
    server.listen(8)
    print(f"listening on 0.0.0.0:{PORT} — point pspssh here")
    if TARGET:
        print(f"forwarding to {TARGET[0]}:{TARGET[1]}, logging both directions")
    else:
        print("answering as a stub server (no target given)")
    sys.stdout.flush()

    while True:
        conn, peer = server.accept()
        handler = proxy if TARGET else serve
        threading.Thread(target=handler, args=(conn, peer), daemon=True).start()


if __name__ == "__main__":
    main()
