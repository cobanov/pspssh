#!/usr/bin/env python3
"""Add a whole-file loop to a RIFF ATRAC3 file.

    tools/add-at3-loop.py in.at3 out.at3

Sony's at3tool writes loop information when given `-wholeloop`, and every
SND0.AT3 the console ships has some. atracdenc writes `fmt `, `fact` and `data`
and nothing else, so ours have never had any.

Whether the XMB requires it is not known. What is known is that this console is
strict about shapes — the version field stays blank because `1.3.6` is not
`XX.YY` — so a file that is valid ATRAC3 but not the shape Sony writes is worth
ruling out before concluding the slot does not work.

The loop is expressed as a RIFF `smpl` chunk covering the whole file, which is
the standard way to say it and the obvious thing `-wholeloop` would produce.
"""
import struct
import sys


def chunks(data):
    """(id, payload) for each top-level chunk, in order."""
    at = 12
    while at + 8 <= len(data):
        cid = data[at:at + 4]
        size = struct.unpack("<I", data[at + 4:at + 8])[0]
        yield cid, data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)


def smpl_chunk(total_samples, sample_rate):
    """A single forward loop over everything, repeating for ever."""
    body = struct.pack(
        "<IIIIIIIII",
        0,                              # manufacturer
        0,                              # product
        1000000000 // sample_rate,      # sample period, nanoseconds
        60,                             # midi unity note
        0,                              # midi pitch fraction
        0,                              # smpte format
        0,                              # smpte offset
        1,                              # loop count
        0,                              # sampler-specific bytes
    )
    body += struct.pack(
        "<IIIIII",
        0,                              # identifier
        0,                              # type: forward
        0,                              # start sample
        max(total_samples - 1, 0),      # end sample
        0,                              # fraction
        0,                              # play count: for ever
    )
    return b"smpl" + struct.pack("<I", len(body)) + body


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    source, target = sys.argv[1], sys.argv[2]
    data = open(source, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{source} is not a RIFF WAVE file")

    rate = None
    total = None
    for cid, payload in chunks(data):
        if cid == b"fmt ":
            rate = struct.unpack("<I", payload[4:8])[0]
        elif cid == b"fact":
            # The first word is the sample count. atracdenc writes it, and it is
            # what the loop end has to be measured against — deriving it from
            # the data size instead would need the frame length and get it
            # wrong for the last, partial frame.
            total = struct.unpack("<I", payload[0:4])[0]
        elif cid == b"smpl":
            raise SystemExit(f"{source} already has a loop")

    if rate is None or total is None:
        raise SystemExit(f"{source} has no fmt or no fact chunk")

    # Inserted before `data`, so a reader that stops once it has the audio still
    # sees it.
    out = bytearray(data[:12])
    for cid, payload in chunks(data):
        if cid == b"data":
            out += smpl_chunk(total, rate)
        size = len(payload)
        out += cid + struct.pack("<I", size) + payload
        if size & 1:
            out += b"\0"

    out[4:8] = struct.pack("<I", len(out) - 8)

    with open(target, "wb") as f:
        f.write(bytes(out))

    print(f"{target}: looped over {total} samples at {rate} Hz, "
          f"{len(out)} bytes")


if __name__ == "__main__":
    main()
