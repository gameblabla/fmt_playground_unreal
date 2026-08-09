# Builds the CD image and boots it in TOWNSEMU as a Marty.
#
# If an audio file is available, the image is built as a mixed-mode
# .bin/.cue - ISO9660 data track plus a Red Book audio track - and the
# payload plays that track as CD-DA (src/common/cdda.[ch]). Otherwise it
# is a plain data-only .iso and the payload falls back to streaming
# CD/MUSIC.PCM through the YM2612 DAC instead. See tools/mkcd.sh and the
# player selection in src/main.c.
#
# Override the audio track with: CDDA=path/to/song.wav ./run.sh
CDDA=${CDDA:-Shattered_Decks_French.wav}

if [ -f "$CDDA" ]; then
    ./tools/mkcd.sh "$CDDA"
    CDIMAGE=output.cue
else
    ./tools/mkcd.sh
    CDIMAGE=output.iso
fi

./Tsugaru_CUI.elf "$PWD/MARTY_ROM/" -TOWNSTYPE MARTY -CD "$CDIMAGE" -NORMALFD -DONTUSEFPU -AUTOSCALE
