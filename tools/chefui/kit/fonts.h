/*
 * fonts.h - the kit's generated LVGL fonts (fonts/gen-fonts.sh). Barlow
 * (b400 Regular, b500 Medium) and Barlow Condensed SemiBold (bc600) with
 * tabular figures frozen in, and Lucide icons (icon_N, glyphs in icons.h).
 * The number is the CSS font size in device pixels. Only the sizes the
 * built screens use exist; see gen-fonts.sh for each one's glyph range.
 */
#ifndef CHEFKIT_FONTS_H
#define CHEFKIT_FONTS_H

#include "lvgl.h"

LV_FONT_DECLARE(chefkit_b400_38)
LV_FONT_DECLARE(chefkit_b400_40)
LV_FONT_DECLARE(chefkit_b400_42)
LV_FONT_DECLARE(chefkit_b500_52)
LV_FONT_DECLARE(chefkit_b500_54)
LV_FONT_DECLARE(chefkit_b500_56)
LV_FONT_DECLARE(chefkit_bc600_32)	/* digits and '-' only */
LV_FONT_DECLARE(chefkit_bc600_40)
LV_FONT_DECLARE(chefkit_bc600_44)	/* digits, ':' and '-' only */
LV_FONT_DECLARE(chefkit_bc600_72)	/* digits, '%' and '-' only */
LV_FONT_DECLARE(chefkit_bc600_96)	/* ASCII titles */
LV_FONT_DECLARE(chefkit_icon_24)
LV_FONT_DECLARE(chefkit_icon_36)
LV_FONT_DECLARE(chefkit_icon_56)
LV_FONT_DECLARE(chefkit_icon_60)
LV_FONT_DECLARE(chefkit_icon_64)
LV_FONT_DECLARE(chefkit_icon_72)
LV_FONT_DECLARE(chefkit_icon_84)
LV_FONT_DECLARE(chefkit_icon_96)

#endif /* CHEFKIT_FONTS_H */
