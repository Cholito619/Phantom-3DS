#!/bin/bash
# Builds an installable .cia so the timer shows up on the HOME menu.
# Run from the project folder (the one with the Makefile) in the MSYS2 window:  bash meta/build_cia.sh
# Needs makerom and bannertool on your PATH (see the steps I gave you).
set -e

TITLE="Phantom"                                # <-- the name shown on the HOME menu
DESC="Speedrun timer with splits and stats"    # <-- shown under the name
AUTHOR="YourName"                              # <-- your name
TARGET=$(basename "$(pwd)")                    # same name make uses for the .elf

make
bannertool makebanner -i meta/banner.png -a meta/audio.wav -o meta/banner.bnr
bannertool makesmdh -s "$TITLE" -l "$DESC" -p "$AUTHOR" -i meta/icon.png -o meta/icon.icn
makerom -f cia -o "$TARGET.cia" -elf "$TARGET.elf" -rsf meta/cia.rsf \
        -icon meta/icon.icn -banner meta/banner.bnr -exefslogo -target t
echo "Done: $TARGET.cia"
