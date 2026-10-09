/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file bflib_sysglyph.h
 *     System-font glyphs rendered on demand into sprite fonts for CJK languages.
 * @par Purpose:
 *     Rendering CJK languages using system ttf fonts, into a bitmap atlas.
 */
/******************************************************************************/
#ifndef BFLIB_SYSGLYPH_H
#define BFLIB_SYSGLYPH_H

#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TbSpriteSheet;
struct TbSprite;

TbBool LbSysGlyphsInit(void);
void LbSysGlyphsSheetLoaded(const struct TbSpriteSheet *sheet, const char *data_fname);
void LbSysGlyphsSheetFreed(const struct TbSpriteSheet *sheet);
TbBool LbSysGlyphsAttached(const struct TbSpriteSheet *sheet);
const struct TbSprite *LbSysGlyphSprite(const struct TbSpriteSheet *sheet, uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif
