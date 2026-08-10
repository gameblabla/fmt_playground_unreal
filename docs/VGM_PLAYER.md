# YM2612 VGM player

The default build boots directly on an FM TOWNS or Marty, loads `MUSIC.FTV`
in full from the CD's ISO9660 filesystem, and plays it through the machine's
YM2612. There is no DOS or runtime filesystem dependency.

## Build and run

```sh
make
./tools/mkcd.sh
./Tsugaru_CUI.elf "$PWD/MARTY_ROM/" -TOWNSTYPE MARTY -CD output.iso \
  -NORMALFD -DONTUSEFPU -AUTOSCALE
```

`make` converts
`artifacts/neo_holy_war/neo_holy_war_trimmed_opn2_only_optimized.vgm`
to `CD/MUSIC.FTV`. The result is about 303 KiB rather than 691 KiB. It stores
YM2612 register writes directly and packs groups of DAC waits into two-bit
codes; playback needs no dictionary, heap allocation, or general-purpose
decompressor.

Genesis F-numbers are retuned from the input VGM's declared YM2612 clock to
the FM TOWNS 8MHz-equivalent clock during conversion. Playback is paced by
PIT channel 1's 307.2kHz hardware clock; its exact 1024/147 ratio to the VGM
44.1kHz timeline avoids CPU-speed calibration and accumulated timing drift.

To convert another file:

```sh
python3 tools/vgm2fmt.py song.vgm CD/MUSIC.FTV
```

Only uncompressed, YM2612-only VGM command streams are accepted. Supported
commands are YM2612 port 0/1 writes, waits, PCM data blocks/seeks, DAC stream
writes, and end. The converter deliberately fails on any other chip or VGM
command instead of producing a partially playable file. Change `VGM_SOURCE`
on the `make` command line to build another input automatically.

Run the exact event-stream regression check with:

```sh
python3 tools/test_vgm2fmt.py
```

For the older graphics test payload instead, build with `VGM_PLAYER=0`.
