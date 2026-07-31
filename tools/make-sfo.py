#!/usr/bin/env python3
"""Write a PARAM.SFO with its key table in the order the format requires.

    tools/make-sfo.py [KEY=VALUE ...] TITLE output.sfo

The argument order is mksfoex's, because build.mak's rule is

    $(MKSFO) $(SFOFLAGS) '$(PSP_EBOOT_TITLE)' $@

and being a drop-in replacement is cheaper than patching the rule.

Values prefixed with ':' are stored as 32-bit integers; everything else is a
UTF-8 string.

## Why not mksfoex

Because it does not sort. It emits the fields given on the command line first,
in the order they were written, and only then its own built-ins alphabetically:

    MEMSIZE, APP_VER, DISC_VERSION, BOOTABLE, CATEGORY, DISC_ID, ...

The SFO format specifies an index table ordered by key, and firmware is entitled
to binary-search it. An unsorted table is one where a lookup may or may not find
a key depending on where it landed — which is exactly the shape of "TITLE shows
but APP_VER does not". Whether this console binary-searches is not something
this project can observe, but producing a malformed file and hoping is not a
position worth defending when the fix is to sort a list.

The stock output is also unsorted, with MEMSIZE ahead of everything. That has
gone unnoticed because nothing before this needed a field that sorts earlier
than MEMSIZE.
"""
import struct
import sys

MAGIC = b"\x00PSF"
VERSION = 0x00000101

FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404


def build(pairs):
    """pairs: list of (key, value, is_int), returned in key order."""
    entries = sorted(pairs, key=lambda p: p[0])

    key_blob = b""
    data_blob = b""
    index = b""

    for key, value, is_int in entries:
        key_offset = len(key_blob)
        key_blob += key.encode("utf-8") + b"\0"

        if is_int:
            payload = struct.pack("<I", int(value))
            length = max_length = 4
            fmt = FMT_INT32
        else:
            payload = value.encode("utf-8") + b"\0"
            length = len(payload)
            # Rounded up to four, as every file Sony ships does. The reader uses
            # `length`, so this is padding rather than meaning — but a field
            # whose reserved size is smaller than its content is a file nothing
            # will read the same way twice.
            max_length = (length + 3) & ~3
            fmt = FMT_UTF8
            payload += b"\0" * (max_length - length)

        index += struct.pack("<HHIII", key_offset, fmt, length, max_length,
                             len(data_blob))
        data_blob += payload

    # The key table is padded so the data table starts on a four-byte boundary.
    key_blob += b"\0" * ((-len(key_blob)) % 4)

    key_table = 20 + len(index)
    data_table = key_table + len(key_blob)
    header = struct.pack("<IIIII", struct.unpack("<I", MAGIC)[0], VERSION,
                         key_table, data_table, len(entries))
    return header + index + key_blob + data_blob


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    target = sys.argv[-1]
    title = sys.argv[-2]
    pairs = [("TITLE", title, False)]

    for argument in sys.argv[1:-2]:
        if "=" not in argument:
            raise SystemExit(f"{argument!r} is not KEY=VALUE")
        key, value = argument.split("=", 1)
        if value.startswith(":"):
            pairs.append((key, value[1:], True))
        else:
            pairs.append((key, value, False))

    seen = set()
    for key, _, _ in pairs:
        if key in seen:
            raise SystemExit(f"{key} given twice")
        seen.add(key)

    with open(target, "wb") as f:
        f.write(build(pairs))

    print(f"{target}: {len(pairs)} keys, sorted")


if __name__ == "__main__":
    main()
