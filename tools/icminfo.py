#!/usr/bin/env python3
"""Inspect a bootable IC Memory Card image (ICMGAME.BIN) at rest.

tools/mkicm.py prints this same information right after it builds an
image, but that output is gone as soon as the terminal scrolls past it.
This reads it back out of the file itself - the IPL header and the asset
directory - for a card that was built earlier, downloaded, or dumped off
real hardware, without needing the ELF or any other build artifact.

    tools/icminfo.py ICMGAME.BIN
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ICM_DEFS = os.path.join(HERE, os.pardir, "src", "boot", "icm_defs.h")

sys.path.insert(0, HERE)
from mkicm import load_defs, human  # noqa: E402


def get32(image, off):
    return struct.unpack_from("<I", image, off)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="card image to inspect (e.g. ICMGAME.BIN)")
    args = ap.parse_args()

    d = load_defs(ICM_DEFS)
    with open(args.image, "rb") as f:
        image = f.read()

    if len(image) < d["FMT_ICM_HDR_END"]:
        raise SystemExit("%s: %d bytes, too short to hold an IPL header"
                          % (args.image, len(image)))

    sig = image[0:4]
    if sig != b"IPL4":
        print("warning: no IPL4 signature at offset 0 (got %r) - "
              "this may not be a bootable card image" % sig, file=sys.stderr)

    payload_off = get32(image, d["FMT_ICM_HDR_PAYLOAD_OFF"])
    payload_bytes = get32(image, d["FMT_ICM_HDR_PAYLOAD_BYTES"])
    load_addr = get32(image, d["FMT_ICM_HDR_LOAD_ADDR"])
    entry = get32(image, d["FMT_ICM_HDR_ENTRY"])
    toc_off = get32(image, d["FMT_ICM_HDR_TOC_OFF"])
    card_bytes = get32(image, d["FMT_ICM_HDR_CARD_BYTES"])
    card_base = get32(image, d["FMT_ICM_HDR_CARD_BASE"])
    window = 0x01000000 if card_base >= 0xC0000000 else 0x00100000

    print("%s: %s card image" % (args.image, human(len(image))))
    print("  card base      0x%08X (%s window, %s)"
          % (card_base, human(window),
             "unbanked" if window >= card_bytes else "banked"))
    print("  card size      %s (header says %s)"
          % (human(len(image)), human(card_bytes)))
    print("  payload        %6d bytes at 0x%06X, loads at 0x%08X, entry 0x%08X"
          % (payload_bytes, payload_off, load_addr, entry))
    print("  directory      at 0x%06X" % toc_off)

    if toc_off + d["FMT_ICM_TOC_HDR_BYTES"] > len(image):
        raise SystemExit("directory offset 0x%X is past the end of the file" % toc_off)

    magic, count = struct.unpack_from("<II", image, toc_off)
    if magic != d["FMT_ICM_TOC_MAGIC"]:
        raise SystemExit("directory at 0x%X has bad magic 0x%08X (expected %r)"
                          % (toc_off, magic, d["FMT_ICM_TOC_MAGIC"].to_bytes(4, "little")))

    print("  %d asset(s):" % count)
    entry_off = toc_off + d["FMT_ICM_TOC_HDR_BYTES"]
    for i in range(count):
        off = entry_off + i * d["FMT_ICM_TOC_ENTRY_BYTES"]
        name = image[off:off + d["FMT_ICM_TOC_NAME_LEN"]].rstrip(b"\0").decode(errors="replace")
        aoff, asize = struct.unpack_from("<II", image, off + d["FMT_ICM_TOC_NAME_LEN"])
        flag = ""
        if aoff + asize > len(image):
            flag = "  ** runs past the end of the file **"
        print("    %-12s %8d bytes at 0x%06X (bank %d)%s"
              % (name, asize, aoff, aoff // window, flag))

    return 0


if __name__ == "__main__":
    sys.exit(main())
