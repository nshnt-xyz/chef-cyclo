#!/bin/sh
# Regenerate the kit's LVGL fonts (*.c here) from src/. Run from anywhere:
#   tools/chefui/kit/fonts/gen-fonts.sh
# Needs python3 with fontTools and node/npx (lv_font_conv is fetched by npx
# at the pinned version). The output is deterministic: the same sources and
# tool versions give byte-identical files (tools/chefui `make font-check`).
#
# Barlow's default figures are proportional and lv_font_conv ignores GSUB,
# so freeze_tnum.py first points the digits at their tabular (tnum) glyphs.
# --no-compress: LV_USE_FONT_COMPRESSED is off in lv_conf.h, and compressed
# glyphs would not render. Only sizes and glyphs the built screens use are
# generated; add a line below for a new size (snap a design size within
# 2 px to an existing one first) and list it in fonts.h.
#
# Sources (sha256 checked below; licences in src/):
#   Barlow-Regular/Medium.ttf, BarlowCondensed-SemiBold.ttf: google/fonts
#     ofl/barlow and ofl/barlowcondensed at 89f5431ff0db (OFL 1.1)
#   lucide.ttf: lucide-static@0.460.0 font/lucide.ttf (ISC); codepoints
#     from that package's font/info.json (see kit/icons.h)
set -eu
cd "$(dirname "$0")"
CONV="npx --yes lv_font_conv@1.5.3"

sha256sum -c --quiet <<'SUMS'
95aa02c7c43096e0dd44d787ba6216864a67157e402adab59b35572e0c1577ea  src/Barlow-Regular.ttf
f8906f762cb73dca441da034bc363b2d8e2e68bc10d5c05e58717646c20cc4b4  src/Barlow-Medium.ttf
7b619d14bc2327509a9ef32b0890f709626f7ecc9ff61191c2a4314c5499d2d9  src/BarlowCondensed-SemiBold.ttf
55e868f053500d4e4298fac5515f2d4f63a10aa7b263b022ff96c126cd9e95dc  src/lucide.ttf
SUMS

# Fixed relative path: lv_font_conv writes its arguments into each file.
rm -rf .tnum
mkdir .tnum
trap 'rm -rf .tnum' EXIT
for f in Barlow-Regular Barlow-Medium BarlowCondensed-SemiBold; do
	python3 -I freeze_tnum.py "src/$f.ttf" ".tnum/$f.ttf" >/dev/null
done

BODY=0x20-0x7E,0xA0-0xFF,0x2013,0x2018-0x201D,0x2026,0x2212
DIGITS=0x30-0x39
TITLE=0x20-0x7E,0x2013,0x2019,0x2026

# name font size range
gen() {
	$CONV --no-compress --no-prefilter --bpp 4 --format lvgl --lv-include lvgl.h \
		--font "$2" --size "$3" -r "$4" --lv-font-name "chefkit_$1" -o "chefkit_$1.c" >/dev/null
	echo "chefkit_$1.c"
}

R=.tnum/Barlow-Regular.ttf
M=.tnum/Barlow-Medium.ttf
C=.tnum/BarlowCondensed-SemiBold.ttf
gen b400_38 $R 38 $BODY		# tile subtitle
gen b400_40 $R 40 $BODY		# help lines under a control
gen b400_42 $R 42 $BODY		# row subtitle
gen b500_52 $M 52 $BODY		# segmented control
gen b500_54 $M 54 $BODY		# tile title
gen b500_56 $M 56 $BODY		# row title
gen bc600_32 $C 32 0x2D,$DIGITS	# battery percentage
gen bc600_40 $C 40 $BODY	# section label
gen bc600_44 $C 44 0x2D,$DIGITS,0x3A	# status bar time
gen bc600_72 $C 72 0x25,0x2D,$DIGITS	# brightness value
gen bc600_96 $C 96 $TITLE	# page title (fixed strings: ASCII only, 218 KB with BODY)

# Icons, only the glyphs used at each size (names in kit/icons.h).
L=src/lucide.ttf
gen icon_24 $L 24 0xE1B4			# zap
gen icon_36 $L 36 0xE1DA,0xE1DB,0xE282,0xE060,0xE1B8,0xE1B9,0xE1AE,0xE1AF	# status bar
gen icon_56 $L 56 0xE122,0xE17C,0xE2B2		# moon sun sun-moon
gen icon_60 $L 60 0xE299			# sun-dim
gen icon_64 $L 64 0xE073			# chevron-right
gen icon_72 $L 72 0xE17C,0xE2B1,0xE223,0xE08B,0xE0F0,0xE0FE	# sun sun-medium vibrate clock hard-drive info
gen icon_84 $L 84 0xE072			# chevron-left
gen icon_96 $L 96 0xE1DA,0xE1DB,0xE282,0xE060,0xE1B8,0xE1B9,0xE1AE,0xE1AF	# hub tiles
