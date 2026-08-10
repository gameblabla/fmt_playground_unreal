#!/usr/bin/env python3
"""Deterministic semantic check for the FTV converter."""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from vgm2fmt import HEADER, TOWNS_VGM_CLOCK, convert  # noqa: E402


def source_events(data):
    pos = 0x34 + struct.unpack_from("<I", data, 0x34)[0]
    pcm = b""
    pcm_pos = 0
    events = []
    while True:
        op = data[pos]
        if op in (0x52, 0x53):
            events.append(("w", op - 0x52, data[pos + 1], data[pos + 2]))
            pos += 3
        elif op == 0x61:
            events.append(("t", struct.unpack_from("<H", data, pos + 1)[0]))
            pos += 3
        elif op in (0x62, 0x63):
            events.append(("t", 735 if op == 0x62 else 882)); pos += 1
        elif 0x70 <= op <= 0x7F:
            events.append(("t", (op & 15) + 1)); pos += 1
        elif 0x80 <= op <= 0x8F:
            events.append(("d", pcm[pcm_pos], op & 15)); pcm_pos += 1; pos += 1
        elif op == 0x67:
            size = struct.unpack_from("<I", data, pos + 3)[0]
            pcm += data[pos + 7:pos + 7 + size]; pos += 7 + size
        elif op == 0xE0:
            pcm_pos = struct.unpack_from("<I", data, pos + 1)[0]; pos += 5
        elif op == 0x66:
            return events
        else:
            raise AssertionError(f"unexpected source opcode {op:02x}")


def converted_events(data):
    magic, hs, clock, samples, pcm_size, off, size, flags = HEADER.unpack_from(data)
    assert magic == b"FTV1" and hs == HEADER.size and flags == 0 and clock
    pcm = data[hs:hs + pcm_size]
    p, end, pcm_pos, events = off, off + size, 0, []
    while p < end:
        op = data[p]; p += 1
        if op == 0: return events, samples
        if op == 1:
            events.append(("w", data[p], data[p + 1], data[p + 2])); p += 3
        elif op == 2:
            events.append(("t", struct.unpack_from("<H", data, p)[0])); p += 2
        elif op == 3:
            pcm_pos = struct.unpack_from("<I", data, p)[0]; p += 4
        elif op == 0x80:
            count = data[p]; p += 1
            packed_size = (count + 3) // 4
            packed, esc = data[p:p + packed_size], p + packed_size
            for i in range(count):
                code = packed[i // 4] >> ((i & 3) * 2) & 3
                wait = (3, 2, 1)[code] if code < 3 else data[esc]
                if code == 3: esc += 1
                events.append(("d", pcm[pcm_pos], wait)); pcm_pos += 1
            p = esc
        else:
            raise AssertionError(f"unexpected FTV opcode {op:02x}")
    raise AssertionError("missing FTV end")


def towns_events(events, source_clock):
    """Reference model for converter-side coherent F-number retuning."""
    high = [[0] * 6 for _ in range(2)]
    result = []
    for event in events:
        if event[0] != "w":
            result.append(event); continue
        _, port, reg, value = event
        normal_hi = 0xA4 <= reg <= 0xA6
        special_hi = port == 0 and 0xAC <= reg <= 0xAE
        normal_lo = 0xA0 <= reg <= 0xA2
        special_lo = port == 0 and 0xA8 <= reg <= 0xAA
        if normal_hi or special_hi:
            index = reg - (0xA4 if normal_hi else 0xA9)
            high[port][index] = value
        elif normal_lo or special_lo:
            index = reg - (0xA0 if normal_lo else 0xA5)
            old = high[port][index]
            block = old >> 3 & 7
            fnum = ((old & 7) << 8) | value
            fnum = min(0x7ff, (fnum * source_clock + TOWNS_VGM_CLOCK // 2) // TOWNS_VGM_CLOCK)
            result.append(("w", port, reg + 4, block << 3 | fnum >> 8))
            result.append(("w", port, reg, fnum & 255))
        else:
            result.append(event)
    return result


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else
                "artifacts/neo_holy_war/neo_holy_war_trimmed_opn2_only_optimized.vgm")
    source = path.read_bytes()
    expected = towns_events(source_events(source), struct.unpack_from("<I", source, 0x2c)[0])
    actual, samples = converted_events(convert(source))
    assert actual == expected
    timeline = sum(e[-1] for e in actual if e[0] in ("t", "d"))
    assert timeline == samples == struct.unpack_from("<I", source, 0x18)[0]
    # Mirror the 307.2kHz PIT scheduler.  307200/44100 = 1024/147 exactly.
    fraction = ticks = exact_numerator = 0
    for event in actual:
        if event[0] not in ("t", "d"):
            continue
        wait = event[-1]
        numerator = fraction + wait * 1024
        ticks += numerator // 147
        fraction = numerator % 147
        exact_numerator += wait * 1024
        assert ticks == exact_numerator // 147
    print(f"OK: {len(actual)} events, {timeline} samples, exact semantic match")


if __name__ == "__main__":
    main()
