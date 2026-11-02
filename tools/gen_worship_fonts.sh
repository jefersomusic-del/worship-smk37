#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# WORSHIP's faces (firmware/src/worship.c): Inter Tight with the Portuguese accents (Latin-1), and a light display
# face for the key. Their own header, so Felucca's fonts (ui_fonts.h) and the FM-1 build stay as they are.
#   tools/gen_worship_fonts.sh OUT.h
set -e
cd "$(dirname "$0")/.."
F="assets/fonts/InterTight[wght].ttf"
EX="+0xC1,0xC0,0xC2,0xC3,0xC7,0xC9,0xCA,0xCD,0xD3,0xD4,0xD5,0xDA,0xB7,0x2026"   # (uppercase only: the texts are)
${PYTHON:-python3} tools/gen_aa_font.py "${1:-build/gen/worship_fonts.h}" --huff WM,WL,XL --kern-min 2 \
    --face "WM=$F@500:15:32-95$EX" \
    --face "WL=$F@300:26:32-95$EX" \
    --face "XL=$F@300:92:32-32+0x23,0x2D,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x41,0x42,0x43,0x44,0x45,0x46,0x47"
