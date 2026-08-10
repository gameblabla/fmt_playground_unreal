#!/bin/sh
# Builds the bootable IC Memory Card image and boots it in TOWNSEMU as a Marty.
#
# The IC card is a boot device like any other on the FM TOWNS, but it is
# not the *first* one the machine tries, so the card boot has to be asked
# for: on real hardware by holding I, C and M while powering on, and here
# by -BOOTKEY ICM. -JEIDA4 attaches the image as a JEIDA4 card, which is
# what makes the megabytes past the first reachable at all (the older
# JEIDA3/SRAM cards this emulator calls -ICM have no bank register).
#
# See docs/ICCARD.md. Override the card size with: ICM_SIZE=8M ./run_icm.sh
set -e

make iccard ${ICM_SIZE:+ICM_SIZE=$ICM_SIZE} "$@"

./Tsugaru_CUI.elf "$PWD/MARTY_ROM/" -TOWNSTYPE MARTY \
    -JEIDA4 ICMGAME.BIN -BOOTKEY ICM -NORMALFD -DONTUSEFPU -AUTOSCALE
