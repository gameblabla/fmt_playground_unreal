#!/usr/bin/env python3
"""Convert a YM2612-only VGM into the small FM TOWNS FTV stream format."""

import argparse
import struct
from pathlib import Path

MAGIC = b"FTV1"
HEADER = struct.Struct("<4s7I")
TOWNS_VGM_CLOCK = 8000000


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def convert(data):
    if data[:4] != b"Vgm ":
        raise ValueError("input is not an uncompressed VGM file")
    version = u32(data, 0x08)
    ym_clock = u32(data, 0x2C) & 0x3FFFFFFF
    if not ym_clock:
        raise ValueError("VGM does not declare a YM2612")
    pos = 0x40 if version < 0x150 else 0x34 + u32(data, 0x34)
    if pos < 0x40 or pos >= len(data):
        raise ValueError("invalid VGM data offset")

    pcm = bytearray()
    stream = bytearray()
    total_samples = 0
    pending_dac = []
    fnum_high = [[0] * 6 for _ in range(2)]

    def flush_dac():
        while pending_dac:
            waits = pending_dac[:255]
            del pending_dac[:len(waits)]
            stream.extend((0x80, len(waits)))
            packed = bytearray((len(waits) + 3) // 4)
            escapes = bytearray()
            for i, wait in enumerate(waits):
                if wait == 3:
                    code = 0
                elif wait == 2:
                    code = 1
                elif wait == 1:
                    code = 2
                else:
                    code = 3
                    escapes.append(wait)
                packed[i // 4] |= code << ((i & 3) * 2)
            stream.extend(packed)
            stream.extend(escapes)

    def emit_wait(samples):
        nonlocal total_samples
        flush_dac()
        total_samples += samples
        while samples:
            chunk = min(samples, 65535)
            stream.append(0x02)
            stream.extend(struct.pack("<H", chunk))
            samples -= chunk

    while pos < len(data):
        cmd = data[pos]
        if cmd in (0x52, 0x53):
            flush_dac()
            if pos + 3 > len(data):
                raise ValueError("truncated YM2612 write")
            port, reg, value = cmd - 0x52, data[pos + 1], data[pos + 2]
            # TOWNS-generated VGMs use an 8MHz-equivalent YM2612 clock.
            # Retune F-numbers from the source clock at conversion time so
            # playback does not run ~4.3% sharp for a 7.670453MHz Genesis VGM.
            # High writes only latch; the paired low write commits frequency,
            # so replace that commit with one coherent transformed pair.
            normal_hi = 0xA4 <= reg <= 0xA6
            special_hi = port == 0 and 0xAC <= reg <= 0xAE
            normal_lo = 0xA0 <= reg <= 0xA2
            special_lo = port == 0 and 0xA8 <= reg <= 0xAA
            if normal_hi or special_hi:
                index = reg - (0xA4 if normal_hi else 0xA9)
                fnum_high[port][index] = value
            elif normal_lo or special_lo:
                index = reg - (0xA0 if normal_lo else 0xA5)
                high = fnum_high[port][index]
                block = (high >> 3) & 7
                fnum = ((high & 7) << 8) | value
                fnum = (fnum * ym_clock + TOWNS_VGM_CLOCK // 2) // TOWNS_VGM_CLOCK
                fnum = min(fnum, 0x7FF)
                high_reg = reg + 4
                stream.extend((0x01, port, high_reg, (block << 3) | (fnum >> 8)))
                stream.extend((0x01, port, reg, fnum & 0xFF))
            else:
                stream.extend((0x01, port, reg, value))
            pos += 3
        elif cmd == 0x61:
            if pos + 3 > len(data):
                raise ValueError("truncated wait")
            emit_wait(struct.unpack_from("<H", data, pos + 1)[0])
            pos += 3
        elif cmd in (0x62, 0x63):
            emit_wait(735 if cmd == 0x62 else 882)
            pos += 1
        elif 0x70 <= cmd <= 0x7F:
            emit_wait((cmd & 15) + 1)
            pos += 1
        elif 0x80 <= cmd <= 0x8F:
            wait = cmd & 15
            total_samples += wait
            pending_dac.append(wait)
            pos += 1
        elif cmd == 0x67:
            flush_dac()
            if pos + 7 > len(data) or data[pos + 1] != 0x66:
                raise ValueError("invalid data block")
            block_type = data[pos + 2]
            size = u32(data, pos + 3)
            if pos + 7 + size > len(data):
                raise ValueError("truncated data block")
            if block_type != 0x00:
                raise ValueError(f"unsupported data block type 0x{block_type:02x}")
            pcm.extend(data[pos + 7:pos + 7 + size])
            pos += 7 + size
        elif cmd == 0xE0:
            flush_dac()
            if pos + 5 > len(data):
                raise ValueError("truncated PCM seek")
            stream.append(0x03)
            stream.extend(data[pos + 1:pos + 5])
            pos += 5
        elif cmd == 0x66:
            flush_dac()
            stream.append(0x00)
            pos += 1
            break
        else:
            raise ValueError(f"unsupported command 0x{cmd:02x} at 0x{pos:x}; input must be YM2612-only")
    else:
        raise ValueError("VGM has no end command")

    declared = u32(data, 0x18)
    if declared and declared != total_samples:
        raise ValueError(f"sample count mismatch: header {declared}, commands {total_samples}")
    header_size = HEADER.size
    return HEADER.pack(MAGIC, header_size, TOWNS_VGM_CLOCK, total_samples,
                       len(pcm), header_size + len(pcm), len(stream), 0) + pcm + stream


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    output = convert(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    ratio = 100.0 * len(output) / args.input.stat().st_size
    print(f"{args.input}: {args.input.stat().st_size} bytes")
    print(f"{args.output}: {len(output)} bytes ({ratio:.1f}%)")


if __name__ == "__main__":
    main()
