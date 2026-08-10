#!/usr/bin/env python3
"""Lay out a bootable FM TOWNS IC Memory Card image.

The card image is the whole game: IPL sector, payload, and every asset,
in one flat file that is written to the card and then simply *is* the
address space the machine sees.

    offset 0        IPL sector (src/boot/icm_ipl.S), 1024 bytes
    offset 1024     payload image: .text/.data/.got, which the IPL
                    copies to RAM, immediately followed by .rodata,
                    which it does not - see src/boot/icm.lds.S
    then            asset directory ("ICMT")
    then            the assets themselves
    then            padding to the requested card size

This is the IC-card analogue of tools/mkcd.sh + tools/inject_ipl.py, and
it patches the IPL for the same reason inject_ipl.py does: where things
ended up is not known until they have been laid out, so nothing about
the layout is a build-time constant of the boot code.

The layout constants come from src/boot/icm_defs.h, parsed rather than
duplicated, so the IPL, the card reader and this script cannot drift
apart. The payload's numbers come out of the linked ELF for the same
reason.
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ICM_DEFS = os.path.join(HERE, os.pardir, "src", "boot", "icm_defs.h")


def load_defs(path):
    """Pick the plain integer #defines out of icm_defs.h."""
    defs = {}
    pat = re.compile(r"^#define\s+(FMT_ICM_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\s*(?:/\*.*)?$")
    with open(path) as f:
        for line in f:
            m = pat.match(line.strip())
            if m:
                defs[m.group(1)] = int(m.group(2), 0)
    return defs


class Elf32:
    """Just enough ELF to read an entry point and a few linker symbols."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 1:
            raise SystemExit("%s: not a 32-bit little-endian ELF" % path)
        (self.entry, _phoff, shoff, _flags, _ehsize, _phentsize, _phnum,
         shentsize, shnum, shstrndx) = struct.unpack_from("<IIIIHHHHHH", self.data, 24)
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            name, stype, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from("<10I", self.data, off)
            self.sections.append(dict(name=name, type=stype, flags=flags, addr=addr,
                                      offset=offset, size=size, link=link,
                                      entsize=entsize))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            end = self.data.index(b"\0", strtab["offset"] + s["name"])
            s["sname"] = self.data[strtab["offset"] + s["name"]:end].decode()

    def symbols(self):
        out = {}
        for s in self.sections:
            if s["sname"] != ".symtab":
                continue
            names = self.sections[s["link"]]
            for i in range(s["size"] // s["entsize"]):
                off = s["offset"] + i * s["entsize"]
                nameoff, value, _size, _info, _other, _shndx = \
                    struct.unpack_from("<IIIBBH", self.data, off)
                end = self.data.index(b"\0", names["offset"] + nameoff)
                name = self.data[names["offset"] + nameoff:end].decode()
                if name:
                    out[name] = value
        return out


def human(n):
    return "%.1f KiB" % (n / 1024.0) if n < 1024 * 1024 else "%.2f MiB" % (n / 1048576.0)


def parse_size(text):
    text = text.strip().upper()
    mult = 1
    if text.endswith("K"):
        mult, text = 1024, text[:-1]
    elif text.endswith("M"):
        mult, text = 1048576, text[:-1]
    return int(text, 0) * mult


def main():
    d = load_defs(ICM_DEFS)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ipl", required=True, help="IPL sector binary (ICM_IPL.BIN)")
    ap.add_argument("--payload", required=True, help="linked payload ELF")
    ap.add_argument("--payload-bin", required=True, help="the same payload as a flat binary")
    ap.add_argument("--out", required=True, help="card image to write")
    ap.add_argument("--size", default="4M", help="card image size (default 4M)")
    ap.add_argument("--card-base", type=lambda s: int(s, 0),
                    default=d["FMT_ICM_CARD_BASE"],
                    help="physical address the card window starts at")
    ap.add_argument("--window", type=lambda s: int(s, 0), default=None,
                    help="bytes of the card visible at once (default: from the base)")
    ap.add_argument("assets", nargs="*",
                    help="asset files, listed in the card directory by upper-cased basename")
    args = ap.parse_args()

    window = args.window
    if window is None:
        window = 0x01000000 if args.card_base >= 0xC0000000 else 0x00100000
    card_bytes = parse_size(args.size)

    ipl = open(args.ipl, "rb").read()
    payload = open(args.payload_bin, "rb").read()
    elf = Elf32(args.payload)
    sym = elf.symbols()

    for need in ("_icm_load_bytes", "_icm_rodata_card", "_icm_rodata_bytes",
                 "_icm_bss_end", "_icm_load_addr"):
        if need not in sym:
            raise SystemExit("%s: linker script did not define %s" % (args.payload, need))

    if len(ipl) > d["FMT_ICM_SECTOR_BYTES"]:
        raise SystemExit(
            "IPL is %d bytes, and only the first %d-byte sector of the card is "
            "guaranteed to be in memory when the boot ROM jumps to it"
            % (len(ipl), d["FMT_ICM_SECTOR_BYTES"]))

    # The payload binary must be exactly the loaded image plus .rodata,
    # contiguous, because that is what the IPL and the card reader both
    # assume when they turn a card offset into an address.
    rodata_off = sym["_icm_rodata_card"] - args.card_base
    expect = rodata_off - d["FMT_ICM_PAYLOAD_OFF"] + sym["_icm_rodata_bytes"]
    if len(payload) != expect:
        raise SystemExit(
            "payload binary is %d bytes but the ELF says %d: the linker script and "
            "the objcopy that flattened it disagree about the layout"
            % (len(payload), expect))
    if sym["_icm_rodata_card"] < args.card_base:
        raise SystemExit("payload .rodata was not linked into card space")

    payload_end = d["FMT_ICM_PAYLOAD_OFF"] + len(payload)

    # The asset directory, and everything the payload was linked against,
    # have to be reachable without touching the bank register: the IPL
    # selects bank 0 and const pointers name fixed addresses inside the
    # window.  Assets themselves may live anywhere - fmt_icm_read() banks
    # across them.
    toc_off = (payload_end + 15) & ~15
    toc_bytes = d["FMT_ICM_TOC_HDR_BYTES"] + len(args.assets) * d["FMT_ICM_TOC_ENTRY_BYTES"]
    if toc_off + toc_bytes > window:
        raise SystemExit(
            "payload plus directory needs %s, more than the %s window at 0x%08X"
            % (human(toc_off + toc_bytes), human(window), args.card_base))

    image = bytearray(b"\xFF" * card_bytes)
    image[0:len(ipl)] = ipl
    image[d["FMT_ICM_PAYLOAD_OFF"]:payload_end] = payload

    entries = []
    at = (toc_off + toc_bytes + 15) & ~15
    for path in args.assets:
        name = os.path.basename(path).upper()
        if len(name) > d["FMT_ICM_TOC_NAME_LEN"]:
            raise SystemExit("asset name %r does not fit in %d characters"
                             % (name, d["FMT_ICM_TOC_NAME_LEN"]))
        blob = open(path, "rb").read()
        if at + len(blob) > card_bytes:
            raise SystemExit(
                "%s does not fit: the image is already %s of the %s card"
                % (path, human(at), human(card_bytes)))
        image[at:at + len(blob)] = blob
        entries.append((name, at, len(blob)))
        at = (at + len(blob) + 15) & ~15

    used = at
    toc = struct.pack("<II", d["FMT_ICM_TOC_MAGIC"], len(entries))
    for name, off, size in entries:
        toc += name.encode().ljust(d["FMT_ICM_TOC_NAME_LEN"], b"\0")
        toc += struct.pack("<II", off, size)
    image[toc_off:toc_off + len(toc)] = toc

    # Patch the IPL's header now that all of that is known.
    def put32(off, value):
        struct.pack_into("<I", image, off, value)

    put32(d["FMT_ICM_HDR_PAYLOAD_OFF"], d["FMT_ICM_PAYLOAD_OFF"])
    put32(d["FMT_ICM_HDR_PAYLOAD_BYTES"], sym["_icm_load_bytes"])
    put32(d["FMT_ICM_HDR_LOAD_ADDR"], sym["_icm_load_addr"])
    put32(d["FMT_ICM_HDR_ENTRY"], elf.entry)
    put32(d["FMT_ICM_HDR_TOC_OFF"], toc_off)
    put32(d["FMT_ICM_HDR_CARD_BYTES"], card_bytes)
    put32(d["FMT_ICM_HDR_CARD_BASE"], args.card_base)

    # Same check the CD build's Makefile rule makes: on the FM TOWNS main
    # RAM stops at 0xC0000, where the FMR VRAM window begins, and .bss
    # that runs past it is not memory - it is a video buffer that reads
    # back whatever the CRTC left there.
    if sym["_icm_bss_end"] > 0xC0000:
        raise SystemExit(
            "payload .bss ends at 0x%X, past the 0xC0000 FMR VRAM window: "
            "it would not be RAM" % sym["_icm_bss_end"])
    if window < card_bytes and window == 0x01000000:
        raise SystemExit("card is larger than the unbanked %s window" % human(window))

    with open(args.out, "wb") as f:
        f.write(image)

    print("IC card image: %s (%s)" % (args.out, human(card_bytes)))
    print("  IPL             %6d bytes at 0x%06X (%d spare in the sector)"
          % (len(ipl), 0, d["FMT_ICM_SECTOR_BYTES"] - len(ipl)))
    print("  payload -> RAM  %6d bytes at 0x%06X, runs at 0x%08X, entry 0x%08X"
          % (sym["_icm_load_bytes"], d["FMT_ICM_PAYLOAD_OFF"],
             sym["_icm_load_addr"], elf.entry))
    print("  const/.rodata   %6d bytes at 0x%06X, read in place at 0x%08X"
          % (sym["_icm_rodata_bytes"], rodata_off, sym["_icm_rodata_card"]))
    print("  directory       %6d bytes at 0x%06X, %d asset(s)"
          % (len(toc), toc_off, len(entries)))
    for name, off, size in entries:
        print("    %-12s %8d bytes at 0x%06X (bank %d)"
              % (name, size, off, off // window))
    print("  used %s of %s, %s free; window %s at 0x%08X"
          % (human(used), human(card_bytes), human(card_bytes - used),
             human(window), args.card_base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
