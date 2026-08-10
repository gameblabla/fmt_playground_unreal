#!/usr/bin/env python3
"""Turn MP3play's 32-band DCT butterfly schedule into compact C."""
import re
import sys

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} MP3.ASM OUTPUT")
src = open(sys.argv[1], encoding="latin1").read()
constants = {}
for name, value in re.findall(r"FIXHR\s+(COS[0-4]_\d+)\s*,[^,]*,\s*([-+]?\d+\.\d+)", src):
    # Q10 keeps every MP3play butterfly coefficient in signed 16 bits,
    # allowing the 386 path to use the low 32-bit product only.
    constants[name] = round(float(value) * 1024)
block = src.split("mp3_dct32_macro macro", 1)[1].split("endm\n;---\nalign code_align\nmp3_dct32_shift_0", 1)[0]

def args(line, op):
    m = re.search(r"@@" + op + r"\s+([0-9]+)\s*,\s*([0-9]+)\s*,\s*([+-])\s*,\s*(COS[0-4]_\d+)\s*,\s*(\d+)", line)
    return m.groups() if m else None

out = []
for raw in block.splitlines():
    line = raw.split(";", 1)[0]
    m = args(line, "BF")
    if m:
        a,b,sign,cos,shift = m
        out.append(f"    mp2_bf(t,{a},{b},{'1' if sign == '+' else '-1'},{constants[cos]},{shift});")
        continue
    m = re.search(r"@@ADD\s+([0-9]+)\s*,\s*([0-9]+)", line)
    if m:
        out.append(f"    t[{m.group(1)}] += t[{m.group(2)}];")
        continue
    m = re.search(r"@@BF([12])\s+([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)", line)
    if m:
        kind,a,b,c,d=m.groups(); q=round(0.7071067811865476*1024)
        out += [f"    mp2_bf(t,{a},{b},1,{q},1);", f"    mp2_bf(t,{c},{d},-1,{q},1);", f"    t[{c}] += t[{d}];"]
        if kind == "2":
            out += [f"    t[{a}] += t[{c}];", f"    t[{c}] += t[{b}];", f"    t[{b}] += t[{d}];"]
        continue
    m = re.search(r"@@OUT\s+([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9-]+)", line)
    if m:
        dst,a,b=m.groups()
        out.append(f"    o[{dst}] = t[{a}]" + (";" if b == "-" else f" + t[{b}];"))

if len([x for x in out if "o[" in x]) != 32:
    raise SystemExit("failed to extract 32 DCT outputs")
with open(sys.argv[2], "w", encoding="ascii") as f:
    f.write("/* Generated from mp3play/MP3.ASM by gen_mp2_dct.py. */\n")
    f.write("static inline void mp2_dct32(int32_t t[32], int32_t o[32]) {\n")
    f.write("    " + "\n    ".join(out) + "\n}\n")
