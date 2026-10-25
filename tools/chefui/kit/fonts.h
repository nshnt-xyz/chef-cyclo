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

LV_FONT_DECLARE(chefkit_b400_26)	/* digits only */
LV_FONT_DECLARE(chefkit_b400_20)	/* a, m, p */
LV_FONT_DECLARE(chefkit_b400_36)	/* ASCII */
LV_FONT_DECLARE(chefkit_b400_38)
LV_FONT_DECLARE(chefkit_b400_40)
LV_FONT_DECLARE(chefkit_b400_42)
LV_FONT_DECLARE(chefkit_b400_44)	/* ASCII and middle dot: build text */
LV_FONT_DECLARE(chefkit_b400_46)	/* ASCII */
LV_FONT_DECLARE(chefkit_b400_48)
LV_FONT_DECLARE(chefkit_b400_52)	/* ASCII: date, capacity unit */
LV_FONT_DECLARE(chefkit_b400_56)	/* " /Wbhkmpr" only (units) */
LV_FONT_DECLARE(chefkit_b400_72)	/* printable ASCII and U+2022 only */
LV_FONT_DECLARE(chefkit_b500_24)	/* ASCII */
LV_FONT_DECLARE(chefkit_b500_32)	/* ASCII */
LV_FONT_DECLARE(chefkit_b500_42)	/* ASCII */
LV_FONT_DECLARE(chefkit_b500_48)
LV_FONT_DECLARE(chefkit_b500_52)
LV_FONT_DECLARE(chefkit_b500_54)
LV_FONT_DECLARE(chefkit_b500_56)
LV_FONT_DECLARE(chefkit_b500_60)
LV_FONT_DECLARE(chefkit_b500_64)	/* ASCII */
LV_FONT_DECLARE(chefkit_bc600_32)	/* digits and '-' only */
LV_FONT_DECLARE(chefkit_bc600_40)
LV_FONT_DECLARE(chefkit_bc600_44)	/* digits, ':' and '-' only */
LV_FONT_DECLARE(chefkit_bc600_48)	/* digits, space and '/' only */
LV_FONT_DECLARE(chefkit_bc600_52)	/* "Pair" only */
LV_FONT_DECLARE(chefkit_bc600_60)	/* ASCII */
LV_FONT_DECLARE(chefkit_bc600_64)	/* digits, " -.BG~" (storage values) */
LV_FONT_DECLARE(chefkit_bc600_68)	/* digits, ",-.", NSEW, degree, en dash */
LV_FONT_DECLARE(chefkit_bc600_76)	/* digits, "-./ms", plus-minus, en dash */
LV_FONT_DECLARE(chefkit_bc600_72)	/* digits, '%' and '-' only */
LV_FONT_DECLARE(chefkit_bc600_80)	/* ASCII */
LV_FONT_DECLARE(chefkit_bc600_88)	/* digits, "-.", en dash */
LV_FONT_DECLARE(chefkit_bc600_96)	/* ASCII titles */
LV_FONT_DECLARE(chefkit_bc600_150)	/* GPS status figures, "chef-cyclo" (gen-fonts.sh) */
LV_FONT_DECLARE(chefkit_bc600_160)	/* ASCII firmware title */
LV_FONT_DECLARE(chefkit_bc600_200)	/* capacity */
LV_FONT_DECLARE(chefkit_bc600_280)	/* clock */
LV_FONT_DECLARE(chefkit_bc600_300)	/* digits, "-.", en dash */
LV_FONT_DECLARE(chefkit_icon_24)
LV_FONT_DECLARE(chefkit_icon_36)
LV_FONT_DECLARE(chefkit_icon_44)
LV_FONT_DECLARE(chefkit_icon_56)
LV_FONT_DECLARE(chefkit_icon_60)
LV_FONT_DECLARE(chefkit_icon_64)
LV_FONT_DECLARE(chefkit_icon_72)
LV_FONT_DECLARE(chefkit_icon_80)
LV_FONT_DECLARE(chefkit_icon_84)
LV_FONT_DECLARE(chefkit_icon_96)
LV_FONT_DECLARE(chefkit_icon_160)
LV_FONT_DECLARE(chefkit_icon_200)

#endif /* CHEFKIT_FONTS_H */
