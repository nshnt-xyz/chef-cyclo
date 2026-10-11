"""Bake the OpenType tnum feature into the cmap so tools that ignore GSUB
(lv_font_conv) get tabular figures: every Unicode cmap entry whose glyph
has a tnum single substitution is pointed at the substitute.
Usage: python3 -I freeze_tnum.py IN.ttf OUT.ttf"""
import sys
from fontTools.ttLib import TTFont

src, dst = sys.argv[1], sys.argv[2]
font = TTFont(src, recalcTimestamp=False)
gsub = font["GSUB"].table
lookups = set()
for rec in gsub.FeatureList.FeatureRecord:
    if rec.FeatureTag == "tnum":
        lookups.update(rec.Feature.LookupListIndex)
subst = {}
for i in sorted(lookups):
    lookup = gsub.LookupList.Lookup[i]
    for st in lookup.SubTable:
        if lookup.LookupType == 7:
            st = st.ExtSubTable
        subst.update(getattr(st, "mapping", {}))
if not subst:
    sys.exit("no tnum single substitutions found")
changed = 0
for table in font["cmap"].tables:
    if not table.isUnicode():
        continue
    for cp, glyph in list(table.cmap.items()):
        if glyph in subst:
            table.cmap[cp] = subst[glyph]
            changed += 1
font.save(dst)
print(f"{dst}: {len(subst)} tnum substitutions, {changed} cmap entries remapped")
