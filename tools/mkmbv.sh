#!/bin/sh
# Converts any video ffmpeg can read into an MBV stream for the Marty player.
#
#   tools/mkmbv.sh input.mkv CD/VIDEO.MBV [extra mbvenc options...]
#
# ffmpeg does the parts it is good at - demuxing, scaling and resampling -
# and hands over raw RGB24 frames and raw unsigned 8-bit mono audio, which is
# all tools/mbvenc.c wants to see.
#
# The geometry is fixed by the machine, not by taste: FMT_MODE_256x240_8BPP is
# a 256x240 8bpp page displayed through 2x CRTC zoom as 512x480, so a 4:3
# source scales straight to 256x240 with no letterboxing and no square-pixel
# correction to do.  The audio rate is the DAC's: the YM2612 channel-6 DAC is
# fed one unsigned byte per sample from a timer-paced loop at 16kHz
# (src/common/dacout.h), and the player takes that stream as its clock, so
# 16kHz mono u8 is not a conversion, it is the target format.
set -e

IN=$1
OUT=$2
[ -n "$IN" ] && [ -n "$OUT" ] || {
    echo "usage: tools/mkmbv.sh input.mkv out.mbv [mbvenc options...]" >&2
    exit 1
}
shift 2

WIDTH=${MBV_WIDTH:-256}
HEIGHT=${MBV_HEIGHT:-240}
FPS=${MBV_FPS:-12}
RATE=${MBV_RATE:-16000}
ENC=${MBVENC:-build/mbvenc}

[ -x "$ENC" ] || {
    echo "mkmbv.sh: $ENC not built - run make build/mbvenc" >&2
    exit 1
}

TMP=$(mktemp -d -t mkmbv-XXXXXX)
trap 'rm -rf "$TMP"' EXIT

ffmpeg -hide_banner -loglevel error -y -i "$IN" -an \
    -vf "fps=$FPS,scale=$WIDTH:$HEIGHT:flags=lanczos" \
    -pix_fmt rgb24 -f rawvideo "$TMP/v.rgb"

# -f u8 is unsigned 8-bit, centred on 0x80 - exactly what the DAC expects,
# so nothing in the player has to bias or scale samples at playback time.
ffmpeg -hide_banner -loglevel error -y -i "$IN" -vn \
    -ac 1 -ar "$RATE" -f u8 "$TMP/a.u8" 2>/dev/null || :
[ -s "$TMP/a.u8" ] || : > "$TMP/a.u8"

"$ENC" -w "$WIDTH" -h "$HEIGHT" -f "$FPS" -r "$RATE" "$@" \
    "$TMP/v.rgb" "$TMP/a.u8" "$OUT"
