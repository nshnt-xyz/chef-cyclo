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
	# Keep new sensor fonts free of trailing blank lines; retain older outputs.
	case "$1" in b400_20|b400_44|b400_52|b400_56|bc600_52|bc600_64|bc600_88|bc600_160|bc600_200|bc600_280|bc600_300|icon_160)
		python3 -I -c 'from pathlib import Path; import sys; p=Path(sys.argv[1]); p.write_bytes(p.read_bytes().rstrip(b"\n") + b"\n")' "chefkit_$1.c" ;;
	esac
	echo "chefkit_$1.c"
}

R=.tnum/Barlow-Regular.ttf
M=.tnum/Barlow-Medium.ttf
C=.tnum/BarlowCondensed-SemiBold.ttf
gen b400_26 $R 26 $DIGITS	# signal bar values
gen b400_20 $R 20 0x61,0x6D,0x70	# status meridiem
gen b400_36 $R 36 $TITLE	# stat keys, legends
gen b400_38 $R 38 $BODY		# tile subtitle
gen b400_40 $R 40 $BODY		# help lines under a control
gen b400_42 $R 42 $BODY		# row subtitle
gen b400_44 $R 44 0x20-0x7E,0xB7	# build line (middle dot)
gen b400_46 $R 46 $TITLE	# empty-state text (fixed strings)
gen b400_48 $R 48 $BODY		# key of a key/value row
gen b400_52 $R 52 $TITLE	# date, capacity unit
gen b400_56 $R 56 0x20,0x2F,0x57,0x62,0x68,0x6B,0x6D,0x70,0x72	# live value unit (bpm W rpm km/h)
gen b400_72 $R 72 0x20-0x7E,0x2022	# password field (printable ASCII, bullet)
gen b500_24 $M 24 $TITLE	# signal bar satellite labels (design: 600)
gen b500_32 $M 32 $TITLE	# sky plot satellite labels (design: 700)
gen b500_42 $M 42 $TITLE	# step labels
gen b500_48 $M 48 $BODY		# value of a key/value row, space key
gen b500_52 $M 52 $BODY		# segmented control
gen b500_54 $M 54 $BODY		# tile title
gen b500_56 $M 56 $BODY		# row title
gen b500_60 $M 60 $BODY		# card title, Join key
gen b500_64 $M 64 $TITLE	# master switch title, keyboard keys
gen bc600_32 $C 32 0x2D,$DIGITS	# battery percentage
gen bc600_40 $C 40 $BODY	# section label
gen bc600_44 $C 44 0x2D,$DIGITS,0x3A	# status bar time
gen bc600_48 $C 48 0x2F,$DIGITS	# "/N" after a count
gen bc600_52 $C 52 0x50,0x61,0x69,0x72	# Pair button
gen bc600_60 $C 60 $TITLE	# outline button
gen bc600_64 $C 64 0x20,0x2D,0x2E,0x42,0x47,0x7E,$DIGITS	# storage values ("12.4 GB", "~")
gen bc600_68 $C 68 0x20,0x2C-0x2E,$DIGITS,0x45,0x4E,0x53,0x57,0xB0,0x2013	# position
gen bc600_76 $C 76 0x20,0x2D-0x2F,$DIGITS,0x6D,0x73,0xB1,0x2013	# stat values
gen bc600_72 $C 72 0x25,0x2D,$DIGITS	# brightness value
gen bc600_80 $C 80 $TITLE	# empty-state title
gen bc600_88 $C 88 0x2D,0x2E,$DIGITS,0x2013	# sensor row value
gen bc600_150 $C 150 0x20,0x2D,0x2E,$DIGITS,0x3A,0x44,0x46,0x4E,0x53,0x61,0x64,0x65,0x66,0x67,0x69,0x6C,0x6D,0x6E,0x6F,0x72,0x74,0x78,0xB1,0x2013	# GPS status figures
gen bc600_96 $C 96 $TITLE	# page title (fixed strings: ASCII only, 218 KB with BODY)
gen bc600_160 $C 160 $TITLE	# firmware title
gen bc600_200 $C 200 0x2D,0x2E,0x7E,$DIGITS	# storage capacity
gen bc600_300 $C 300 0x2D,0x2E,$DIGITS,0x2013	# live sensor value
gen bc600_280 $C 280 0x2D,0x3A,$DIGITS	# clock digits

# Icons, only the glyphs used at each size (names in kit/icons.h).
L=src/lucide.ttf
gen icon_24 $L 24 0xE1B4			# zap
gen icon_36 $L 36 0xE1DA,0xE1DB,0xE282,0xE060,0xE1B8,0xE1B9,0xE1AE,0xE1AF	# status bar
gen icon_44 $L 44 0xE149,0xE10D		# refresh-cw loader
gen icon_56 $L 56 0xE122,0xE17C,0xE2B2,0xE10F	# moon sun sun-moon lock
gen icon_60 $L 60 0xE299,0xE18E			# sun-dim trash-2
gen icon_64 $L 64 0xE073,0xE1E4,0xE0B2,0xE1A9		# chevron-right arrow-big-up delete volume
gen icon_72 $L 72 0xE17C,0xE2B1,0xE223,0xE08B,0xE0F0,0xE0FE,0xE5FC,0xE5FD,0xE5FE,0xE141,0xE226,0xE07A,0xE0BE,0xE0BF,0xE44C,0xE372,0xE1B4,0xE1BF,0xE1AB	# sun sun-medium vibrate clock hard-drive info wifi-high wifi-low wifi-zero plus circle-check circle eye eye-off satellite heart-pulse zap gauge volume-2
gen icon_80 $L 80 0xE1AE			# wifi
gen icon_84 $L 84 0xE072,0xE1B2			# chevron-left x
gen icon_160 $L 160 0xE372,0xE1B4,0xE1BF	# heart-pulse zap gauge (pairing, live card)
gen icon_200 $L 200 0xE1AF,0xE282,0xE1B9	# wifi-off locate-off bluetooth-off (empty states)
gen icon_96 $L 96 0xE1DA,0xE1DB,0xE282,0xE060,0xE1B8,0xE1B9,0xE1AE,0xE1AF	# hub tiles
