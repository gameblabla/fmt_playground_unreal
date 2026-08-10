#!/usr/bin/env python3
"""Translate MP3play's delta-coded fast synthesis window into a C table."""
import re
import sys

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} MP3.ASM OUTPUT")

text = open(sys.argv[1], encoding="latin1").read()
block = text.split("mp3_synth_win_src:", 1)[1].split("mp3_slen_table:", 1)[0]
values = []
for line in block.splitlines():
    if "@@def" not in line:
        continue
    line = line.split(";", 1)[0]
    values.extend(int(v) for v in re.findall(r"(?<![\w@])-?\d+", line))
if len(values) != 256:
    raise SystemExit(f"expected 256 MP3play window values, got {len(values)}")

# MP3play's fast MARTY-friendly path uses WFRAC=11.  Its source macro first
# stores negative deltas, then init reconstructs, signs, mirrors, and swaps.
win = [0] * 512
previous = 0
accumulator = 0
for pos, value in enumerate(values, 1):
    value //= 32
    encoded_delta = -(value - previous)
    previous = value
    accumulator += encoded_delta
    win[512 - pos] = accumulator
    win[pos] = accumulator if pos % 64 == 0 else -accumulator
for i in range(512):
    if i & 0x30:
        win[i] = -win[i]
for i in range(512):
    sub = (i & 63) - 17
    if 0 <= sub <= 14:
        win[i], win[i + 32] = win[i + 32], win[i]

with open(sys.argv[2], "w", encoding="ascii") as out:
    out.write("/* Generated from mp3play/MP3.ASM by gen_mp2_window.py. */\n")
    out.write("static const int16_t mp2_synth_win_q11[512] = {\n")
    for i in range(0, 512, 16):
        out.write("    " + ", ".join(str(x) for x in win[i:i+16]) + ",\n")
    out.write("};\n")
