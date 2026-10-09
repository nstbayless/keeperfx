/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file bflib_sysglyph.cpp
 *     System-font glyphs rendered on demand into sprite fonts for CJK languages.
 * @par Purpose:
 *     See bflib_sysglyph.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "bflib_sysglyph.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "bflib_fileio.h"
#include "config.h"
#include "config_keeperfx.h"
#include "globals.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "front_simple.h"
#include "vidfade.h"
extern "C" {
#include "value_util.h"
}
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <new>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>
#elif defined(__linux__)
#include <fontconfig/fontconfig.h>
#endif
#include "post_inc.h"

#define SG_MAX_GLYPHS 2048
#define SG_GLYPH_DATA_SIZE (400 * 1024) /* 400 kb per font */
#define SG_TOP_BITS 9
#define SG_MID_BITS 6
#define SG_LEAF_BITS 6
#define SG_TOP_SIZE (1 << SG_TOP_BITS)
#define SG_MID_SIZE (1 << SG_MID_BITS)
#define SG_LEAF_SIZE (1 << SG_LEAF_BITS)
#define SG_MAX_CODEPOINT 0x1FFFFF
/** Leaf entries: 0 = TBA; SG_MISSING = no system glyph; else slot + 1. */
#define SG_MISSING 0xFFFF
#define SG_MAX_TYPEFACES 8
#define SG_CULL_BELOW 80
#define SG_PREFERRED_WEIGHT 400

namespace {

struct Rgb { float r, g, b; };

struct SgLanguageCfg {
    uint32_t reference = 0x56FD;
    std::vector<std::string> fonts; // family names, in order of preference
};

struct SgRenderCfg {
    int glyph_height = 0;
    int bottom_gap = 0;
    int cell_width = 0;
    std::vector<float> gradient_pos;
    std::vector<Rgb> gradient_colour;
    Rgb shadow = {0, 0, 0};
    int shadow_dx = 0;
    int shadow_dy = 0;
};

typedef uint16_t SgLeaf[SG_LEAF_SIZE];

struct SgSystemTypeface {
    /** The classic sprite font these glyphs extend */
    const TbSpriteSheet *sheet;
    /** render_<name>, the key of its g_renders entry */
    const char *name;
    TTF_Font *font;
    int cell_w;
    int cell_h;
    int dy;
    const SgRenderCfg *cfg;
    unsigned char *scratch; // truecolour glyph rendering surface
    unsigned char shadow; // palette index of cfg->shadow
    SgLeaf **trie[SG_TOP_SIZE]; // (9-bit pointer table) -> (6-bit pointer table) -> (6-bit u16 glyph index table) -> <relevant glyph data>
    TbSprite *sprites; // glyph sprites (points into 'data')
    unsigned char *data; // general-purpose glyph data storage buffer
    size_t data_used; // bytes of data filled, glyph after glyph
    size_t used; // how many glyphs generated so far
    bool full;
};

SgLanguageCfg g_lang;
std::map<std::string, SgRenderCfg> g_renders;
std::shared_mutex g_lock;
SgSystemTypeface g_faces[SG_MAX_TYPEFACES];

/******************************************************************************/
// Config parsing

std::string to_lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool parse_colour(const char *s, Rgb &out)
{
    int r, g, b, end = 0;
    if (std::sscanf(s, "%d %d %d %n", &r, &g, &b, &end) != 3 || s[end] != '\0')
        return false;
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
        return false;
    out = Rgb{(float)r, (float)g, (float)b};
    return true;
}

TTF_Font *open_font_file(const char *path, int face, float size)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0)
        return NULL;
    SDL_SetStringProperty(props, TTF_PROP_FONT_CREATE_FILENAME_STRING, path);
    SDL_SetFloatProperty(props, TTF_PROP_FONT_CREATE_SIZE_FLOAT, size);
    SDL_SetNumberProperty(props, TTF_PROP_FONT_CREATE_FACE_NUMBER, face);
    TTF_Font *font = TTF_OpenFontWithProperties(props);
    SDL_DestroyProperties(props);
    return font;
}

TTF_Font *open_family(const std::string &family, float size);

void read_render_section(const char *sect_name, VALUE *sect)
{
    static const char render_prefix[] = "render_";
    if (value_type(sect) != VALUE_DICT || std::strncmp(sect_name, render_prefix, sizeof(render_prefix) - 1) != 0)
        return;
    const std::string key = sect_name + sizeof(render_prefix) - 1;
    SgRenderCfg r;
    CONDITIONAL_ASSIGN_INT(sect, "glyph_height", r.glyph_height);
    CONDITIONAL_ASSIGN_INT(sect, "bottom_gap", r.bottom_gap);
    CONDITIONAL_ASSIGN_INT(sect, "cell_width", r.cell_width);
    VALUE *shadow = value_dict_get(sect, "shadow");
    if (value_type(shadow) == VALUE_STRING) {
        if (parse_colour(value_string(shadow), r.shadow)) {
            r.shadow_dx = r.shadow_dy = 1;
            CONDITIONAL_ASSIGN_INT(sect, "shadow_dx", r.shadow_dx);
            CONDITIONAL_ASSIGN_INT(sect, "shadow_dy", r.shadow_dy);
        } else {
            WARNLOG("sysglyphs.toml: [render_%s] invalid shadow colour \"%s\"", key.c_str(), value_string(shadow));
        }
    }
    if (r.shadow_dx < 0 || r.shadow_dy < 0) {
        WARNLOG("sysglyphs.toml: [render_%s] shadow offset must be nonnegative", key.c_str());
        r.shadow_dx = r.shadow_dy = 0;
    }
    VALUE *gradient = value_dict_get(sect, "gradient");
    for (size_t i = 0; i < value_array_size(gradient); i++) {
        VALUE *v = value_array_get(gradient, i);
        const char *stop = value_type(v) == VALUE_STRING ? value_string(v) : "";
        float pct;
        int n = 0;
        Rgb c;
        if (std::sscanf(stop, "%f %n", &pct, &n) != 1 || !parse_colour(stop + n, c)) {
            WARNLOG("sysglyphs.toml: invalid gradient pos \"%s\"", stop);
        } else if (!r.gradient_pos.empty() && pct / 100.0f < r.gradient_pos.back()) {
            WARNLOG("sysglyphs.toml: gradient pos \"%s\" is out of order", stop);
        } else {
            r.gradient_pos.push_back(pct / 100.0f);
            r.gradient_colour.push_back(c);
        }
    }
    if (r.glyph_height <= 0 || r.cell_width <= 0 || r.gradient_pos.empty()) {
        WARNLOG("sysglyphs.toml: [render_%s] needs glyph_height, cell_width and gradient", key.c_str());
        return;
    }
    g_renders[to_lower(key)] = r;
}

int render_section_visitor(const VALUE *name, VALUE *sect, void *ctx)
{
    try {
        read_render_section(value_string(name), sect);
    } catch (const std::bad_alloc &) {
        *(bool *)ctx = true;
        return 1;
    }
    return 0;
}

void load_config(int lang_id)
{
    g_lang = SgLanguageCfg();
    g_renders.clear();
    if (!is_dbc_language(lang_id))
        return;

    std::string lang_fname = std::string("sysglyphs_") + get_language_lwrstr(lang_id) + ".toml";
    if (!LbFileExists(prepare_file_path(FGrp_FxData, lang_fname.c_str())))
        lang_fname = "sysglyphs_default.toml";
    VALUE lang;
    if (!load_toml_file(prepare_file_path(FGrp_FxData, lang_fname.c_str()), &lang, 0)) {
        WARNLOG("sysglyphs: cannot load %s", lang_fname.c_str());
        return;
    }
    VALUE *ref = value_dict_get(&lang, "reference");
    if (ref != NULL && value_type(ref) == VALUE_STRING)
        g_lang.reference = (uint32_t)std::strtoul(value_string(ref), NULL, 16);
    VALUE *fonts = value_dict_get(&lang, "fonts");
    for (size_t i = 0; i < value_array_size(fonts); i++) {
        VALUE *v = value_array_get(fonts, i);
        if (value_type(v) == VALUE_STRING)
            g_lang.fonts.emplace_back(value_string(v));
    }
    value_fini(&lang);
    if (g_lang.fonts.empty()) {
        WARNLOG("sysglyphs: no fonts listed in %s", lang_fname.c_str());
        return;
    }

    const char *fname = prepare_file_path(FGrp_FxData, "sysglyphs.toml");
    VALUE root;
    if (!load_toml_file(fname, &root, 0))
        return;

    bool out_of_memory = false;
    value_dict_walk_sorted(&root, render_section_visitor, &out_of_memory);
    value_fini(&root);
    if (out_of_memory)
        throw std::bad_alloc();
}

/******************************************************************************/
// OS font lookup

#if defined(_WIN32)

template <class T> void release(T *&p)
{
    if (p != NULL)
        p->Release();
    p = NULL;
}

TTF_Font *open_family(const std::string &family, float size)
{
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, NULL, 0);
    if (wlen <= 0)
        return NULL;
    std::vector<wchar_t> wfamily((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, wfamily.data(), wlen);

    static IDWriteFactory *factory = NULL;
    if (factory == NULL &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory))) {
        factory = NULL;
        WARNLOG("sysglyphs: DirectWrite not available; no system fonts");
        return NULL;
    }
    IDWriteFontCollection *coll = NULL;
    IDWriteFontFamily *fam = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;
    IDWriteFontFile *file = NULL;
    IDWriteFontFileLoader *loader = NULL;
    IDWriteLocalFontFileLoader *local = NULL;
    std::string path_utf8;
    int face_index = 0;
    UINT32 index = 0, nfiles = 1;
    BOOL exists = FALSE;
    const void *key = NULL;
    UINT32 key_size = 0, path_len = 0;
    if (SUCCEEDED(factory->GetSystemFontCollection(&coll, FALSE)) &&
        SUCCEEDED(coll->FindFamilyName(wfamily.data(), &index, &exists)) && exists &&
        SUCCEEDED(coll->GetFontFamily(index, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)SG_PREFERRED_WEIGHT, DWRITE_FONT_STRETCH_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&face)) &&
        SUCCEEDED(face->GetFiles(&nfiles, &file)) && file != NULL &&
        SUCCEEDED(file->GetReferenceKey(&key, &key_size)) &&
        SUCCEEDED(file->GetLoader(&loader)) &&
        SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&local)) &&
        SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &path_len))) {
        std::vector<wchar_t> path((size_t)path_len + 1);
        if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, path.data(), path_len + 1))) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, path.data(), -1, NULL, 0, NULL, NULL);
            if (n > 1) {
                path_utf8.assign((size_t)n - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, path.data(), -1, &path_utf8[0], n, NULL, NULL);
                face_index = (int)face->GetIndex();
            }
        }
    }
    release(local);
    release(loader);
    release(file);
    release(face);
    release(font);
    release(fam);
    release(coll);
    return path_utf8.empty() ? NULL : open_font_file(path_utf8.c_str(), face_index, size);
}

#elif defined(__linux__)

TTF_Font *open_family(const std::string &family, float size)
{
    FcPattern *pat = FcPatternCreate();
    if (pat == NULL)
        return NULL;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family.c_str());
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(SG_PREFERRED_WEIGHT));
    FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcFontSet *set = FcFontSort(NULL, pat, FcFalse, NULL, &res);
    TTF_Font *font = NULL;
    for (int i = 0; set != NULL && i < set->nfont && font == NULL; i++) {
        const FcPattern *p = set->fonts[i];
        FcChar8 *file = NULL;
        FcChar8 *fam = NULL;
        int index = 0;
        FcBool variable = FcFalse;
        bool same_family = false;
        for (int f = 0; FcPatternGetString(p, FC_FAMILY, f, &fam) == FcResultMatch; f++)
            same_family |= FcStrCmpIgnoreCase(fam, (const FcChar8 *)family.c_str()) == 0;
        if (!same_family)
            continue;
        if (FcPatternGetBool(p, FC_VARIABLE, 0, &variable) == FcResultMatch && variable)
            continue;
        if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch)
            continue;
        FcPatternGetInteger(p, FC_INDEX, 0, &index);
        font = open_font_file((const char *)file, index, size);
    }
    if (set != NULL)
        FcFontSetDestroy(set);
    FcPatternDestroy(pat);
    return font;
}

#else

TTF_Font *open_family(const std::string &, float)
{
    // TODO -- other OSes
    return NULL;
}

#endif

/******************************************************************************/
// Typefaces

void flush_sdl_ttf_cache(TTF_Font *font)
{
    TTF_SetFontHinting(font, TTF_HINTING_NONE);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
}

// select first valid font to contain reference glyph,
// and select the size such that the reference glyph is
// the configured number of pixels tall.
TTF_Font *choose_size_and_open_font(const SgRenderCfg &r)
{
    for (const auto &family : g_lang.fonts) {
        TTF_Font *font = open_family(family, (float)r.glyph_height);
        if (font == NULL) {
            continue;
        }
        if (!TTF_FontHasGlyph(font, g_lang.reference)) {
            TTF_CloseFont(font);
            continue;
        }
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
        float size = (float)r.glyph_height;
        for (int iter = 0; iter < 4; iter++) {
            int minx, maxx, miny, maxy, adv;
            if (!TTF_GetGlyphMetrics(font, g_lang.reference, &minx, &maxx, &miny, &maxy, &adv) || maxy <= miny)
                break;
            const int ink = maxy - miny;
            if (ink == r.glyph_height)
                break;
            size = size * (float)r.glyph_height / (float)ink;
            TTF_SetFontSize(font, size);
        }
        return font;
    }

    SYNCDBG(7, "sysglyphs: No valid system font found.");
    return NULL;
}

Rgb gradient_at(const SgRenderCfg &r, float t)
{
    const std::vector<float> &pos = r.gradient_pos;
    const std::vector<Rgb> &col = r.gradient_colour;
    if (t <= pos.front()) return col.front();
    for (size_t i = 1; i < pos.size(); i++) {
        if (t <= pos[i]) {
            const float span = pos[i] - pos[i - 1];
            const float f = span > 0 ? (t - pos[i - 1]) / span : 1.0f;
            const Rgb &a = col[i - 1], &b = col[i];
            return Rgb{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        }
    }
    return col.back();
}

unsigned char palette_index(const Rgb &c)
{
    const int shift = 8 - COLOUR_TABLE_BITS_PER_VALUE;
    const int r = std::clamp((int)c.r, 0, 255) >> shift;
    const int g = std::clamp((int)c.g, 0, 255) >> shift;
    const int b = std::clamp((int)c.b, 0, 255) >> shift;
    const unsigned char idx = colours[r][g][b];
    return idx == 0 ? 1 : idx;
}

void free_typeface(SgSystemTypeface *tf)
{
    if (tf->sprites != NULL)
        RendererForgetSprites(tf->sprites, SG_MAX_GLYPHS);
    free(tf->sprites);
    free(tf->data);
    for (int t = 0; t < SG_TOP_SIZE; t++) {
        if (tf->trie[t] == NULL)
            continue;
        for (int m = 0; m < SG_MID_SIZE; m++)
            free(tf->trie[t][m]);
        free(tf->trie[t]);
    }
    if (tf->font != NULL)
        TTF_CloseFont(tf->font);
    free(tf->scratch);
    *tf = SgSystemTypeface{};
}

bool open_typeface(SgSystemTypeface *tf, const SgRenderCfg &r)
{
    const TbSprite *space = get_sprite(tf->sheet, 1);
    if (space == NULL)
        return false;
    const int cell_h = space->SHeight;
    const int cell_w = r.cell_width;
    if (cell_w < 2 || cell_w > 255 || cell_h <= 0 || cell_h > 255) {
        WARNLOG("sysglyphs: cell %dx%d out of range", cell_w, cell_h);
        return false;
    }
    if (!TTF_WasInit() && !TTF_Init()) {
        WARNLOG("sysglyphs: TTF_Init failed: %s", SDL_GetError());
        return false;
    }

    tf->cell_w = cell_w;
    tf->cell_h = cell_h;
    tf->sprites = (TbSprite *)calloc(SG_MAX_GLYPHS, sizeof(TbSprite));
    if (tf->sprites == NULL) {
        ERRORLOG("sysglyphs: out of memory (typeface tables)");
        return false;
    }

    tf->font = choose_size_and_open_font(r);
    if (tf->font == NULL) {
        WARNLOG("sysglyphs: no suitable font for for %s found; using fallback",
                get_language_lwrstr(install_info.lang_id));
        return false;
    }
    int ref_minx, ref_maxx, ref_miny, ref_maxy, ref_adv;
    TTF_GetGlyphMetrics(tf->font, g_lang.reference, &ref_minx, &ref_maxx, &ref_miny, &ref_maxy, &ref_adv);
    const int ascent = TTF_GetFontAscent(tf->font);
    flush_sdl_ttf_cache(tf->font);

    tf->scratch = (unsigned char *)malloc((size_t)cell_w * cell_h);
    tf->data = (unsigned char *)malloc(SG_GLYPH_DATA_SIZE);
    if (tf->scratch == NULL || tf->data == NULL) {
        ERRORLOG("sysglyphs: out of memory (glyph data)");
        return false;
    }

    // positioning
    tf->cfg = &r;
    tf->dy = cell_h - r.bottom_gap - ascent + ref_miny;
    if (r.shadow_dx != 0 || r.shadow_dy != 0)
        tf->shadow = palette_index(r.shadow);
    SYNCLOG("System glyph typeface \"%s\": %s %s, %dx%d cells, rendered on demand",
            tf->name, TTF_GetFontFamilyName(tf->font), TTF_GetFontStyleName(tf->font), cell_w, cell_h);
    return true;
}

// gets entry for codepoint, doesn't insert.
uint16_t find_entry(const SgSystemTypeface *tf, uint32_t codepoint)
{
    SgLeaf **mid = tf->trie[codepoint >> (SG_MID_BITS + SG_LEAF_BITS)];
    if (mid == NULL)
        return 0;
    const SgLeaf *leaf = mid[(codepoint >> SG_LEAF_BITS) & (SG_MID_SIZE - 1)];
    if (leaf == NULL)
        return 0;
    return (*leaf)[codepoint & (SG_LEAF_SIZE - 1)];
}

// gets pointer to entry for codepoint, adding trie entries if needed.
// NULL on failure.
uint16_t *trie_entry(SgSystemTypeface *tf, uint32_t codepoint)
{
    const uint32_t t = codepoint >> (SG_MID_BITS + SG_LEAF_BITS);
    const uint32_t m = (codepoint >> SG_LEAF_BITS) & (SG_MID_SIZE - 1);
    const uint32_t l = codepoint & (SG_LEAF_SIZE - 1);
    if (tf->trie[t] == NULL) {
        tf->trie[t] = (SgLeaf **)calloc(SG_MID_SIZE, sizeof(SgLeaf *));
        if (tf->trie[t] == NULL)
            return NULL;
    }
    if (tf->trie[t][m] == NULL) {
        tf->trie[t][m] = (SgLeaf *)calloc(1, sizeof(SgLeaf));
        if (tf->trie[t][m] == NULL)
            return NULL;
    }
    return &(*tf->trie[t][m])[l];
}

size_t encode_rle(const unsigned char *px, int pitch, int w, int h, unsigned char *out, size_t room)
{
    size_t n_out = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = &px[(size_t)y * pitch];
        int x = 0;
        while (x < w) {
            const bool transp = row[x] == 0;
            int n = 0;
            while (x + n < w && (row[x + n] == 0) == transp && n < 127)
                n++;
            const size_t len = transp ? 1 : 1 + (size_t)n;
            if (n_out + len > room)
                return 0;
            if (transp) {
                out[n_out++] = (unsigned char)(signed char)(-n);
            } else {
                out[n_out++] = (unsigned char)n;
                std::memcpy(out + n_out, row + x, (size_t)n);
                n_out += (size_t)n;
            }
            x += n;
        }
        if (n_out + 1 > room)
            return 0;
        out[n_out++] = 0;
    }
    return n_out;
}

// characters to be drawn centered. Most CJK characters should be centered to look nice.
// (but some characters like punctuation should not be centered.)
bool is_centred_char(uint32_t codepoint)
{
    static std::pair<uint32_t, uint32_t> ranges[] = {
        {0x2E80, 0x2FDF},   // CJK radicals
        {0x3005, 0x3007},   // misc. CJK
        {0x3041, 0x3096},   // hiragana
        {0x309D, 0x309F},   // misc. hiragana
        {0x30A1, 0x30FA},   // katakana
        {0x30FC, 0x30FF},   // misc. katakana
        {0x3105, 0x312F},   // bopomofo
        {0x31A0, 0x31BF},   // bopomofo ex.
        {0x31F0, 0x31FF},   // irankarapte!
        {0x3400, 0x4DBF},   // CJK +A
        {0x4E00, 0x9FFF},   // CJK
        {0xF900, 0xFAFF},   // CJK compatibility ideographs
        {0xFF10, 0xFF19},   // fullwidth digits
        {0xFF21, 0xFF3A},   // fullwidth uppercase
        {0xFF41, 0xFF5A},   // fullwidth lowercase
        {0x1B000, 0x1B16F}, // kana ex.
        {0x20000, 0x323AF}, // CJK +B~H
    };
    for (const auto &r : ranges)
        if (codepoint >= r.first && codepoint <= r.second)
            return true;
    return false;
}

int render_glyph(SgSystemTypeface *tf, uint32_t codepoint)
{
    if (tf->used >= SG_MAX_GLYPHS) {
        WARNLOG("sysglyphs: all %d glyphs of \"%s\" used; further glyphs will use fallback", SG_MAX_GLYPHS, tf->name);
        tf->full = true;
        return -1;
    }
    if (!TTF_FontHasGlyph(tf->font, codepoint))
        return -1;

    // failure -> treat as missing
    static const SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderGlyph_Blended(tf->font, codepoint, white);
    flush_sdl_ttf_cache(tf->font);
    if (surf == NULL)
        return -1;
    if (surf->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(surf);
        surf = conv;
        if (surf == NULL)
            return -1;
    }

    // monospace centering
    int minx = 0, maxx = 0, miny = 0, maxy = 0, adv = 0;
    TTF_GetGlyphMetrics(tf->font, codepoint, &minx, &maxx, &miny, &maxy, &adv);
    flush_sdl_ttf_cache(tf->font);
    int ink_left = surf->w, ink_right = -1;
    for (int sy = 0; sy < surf->h; sy++) {
        const uint32_t *src = (const uint32_t *)((const unsigned char *)surf->pixels + (size_t)sy * surf->pitch);
        for (int sx = 0; sx < surf->w; sx++) {
            if ((src[sx] >> 24) >= SG_CULL_BELOW) {
                ink_left = std::min(ink_left, sx);
                ink_right = std::max(ink_right, sx);
            }
        }
    }
    const int pitch = tf->cell_w;
    const int room = pitch - 1;

    const int origin = std::max(0, -minx);
    int dx;
    if (ink_right < ink_left) {
        dx = -origin;
    } else if (is_centred_char(codepoint)) {
        dx = (room - (ink_right - ink_left + 1)) / 2 - ink_left;
    } else {
        dx = -origin;
        if (ink_right >= ink_left && ink_right + dx >= room)
            dx = std::max(room - 1 - ink_right, -ink_left);
    }

    // gradient
    const SgRenderCfg &cfg = *tf->cfg;
    std::memset(tf->scratch, 0, (size_t)pitch * tf->cell_h);
    for (int sy = 0; sy < surf->h; sy++) {
        const int y = sy + tf->dy;
        if (y < 0 || y >= tf->cell_h)
            continue;
        const int band_top = tf->cell_h - cfg.bottom_gap - cfg.glyph_height;
        const float t = (cfg.glyph_height > 1) ? (float)(y - band_top) / (float)(cfg.glyph_height - 1) : 0.0f;
        const Rgb c = gradient_at(cfg, t);
        const uint32_t *src = (const uint32_t *)((const unsigned char *)surf->pixels + (size_t)sy * surf->pitch);
        for (int sx = 0; sx < surf->w; sx++) {
            const int x = sx + dx;
            if (x < 0 || x >= room)
                continue;
            const unsigned v = src[sx] >> 24;
            if (v < SG_CULL_BELOW)
                continue;
            const float k = v / 255.0f;
            const unsigned char idx = palette_index(Rgb{c.r * k, c.g * k, c.b * k});
            const unsigned char *rgb = &engine_palette[idx * 3];
            if (rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0)
                tf->scratch[(size_t)y * pitch + x] = idx;
        }
    }
    SDL_DestroySurface(surf);

    // shadow
    if (cfg.shadow_dx != 0 || cfg.shadow_dy != 0) {
        for (int y = tf->cell_h - 1; y >= cfg.shadow_dy; y--) {
            const int sy = y - cfg.shadow_dy;
            for (int x = pitch - 1; x >= cfg.shadow_dx; x--) {
                const int sx = x - cfg.shadow_dx;
                unsigned char *px = &tf->scratch[(size_t)y * pitch + x];
                if (*px == 0 && tf->scratch[(size_t)sy * pitch + sx] != 0)
                    *px = tf->shadow;
            }
        }
    }

    unsigned char *data = tf->data + tf->data_used;
    const size_t len = encode_rle(tf->scratch, pitch, pitch, tf->cell_h, data, SG_GLYPH_DATA_SIZE - tf->data_used);
    if (len == 0) {
        WARNLOG("sysglyphs: glyph data of \"%s\" full after %d glyphs; further glyphs will use fallback",
                tf->name, (int)tf->used);
        tf->full = true;
        return -1;
    }
    tf->data_used += len;
    const int slot = (int)tf->used++;
    TbSprite *spr = &tf->sprites[slot];
    spr->Data = data;
    spr->SWidth = tf->cell_w;
    spr->SHeight = tf->cell_h;
    return slot;
}

uint16_t add_glyph(SgSystemTypeface *tf, uint32_t codepoint)
{
    if (tf->full)
        return 0;
    uint16_t *entry = trie_entry(tf, codepoint);
    if (entry == NULL) {
        ERRORLOG("sysglyphs: out of memory (glyph index)");
        tf->full = true;
        return 0;
    }
    if (*entry == 0) {
        const int slot = render_glyph(tf, codepoint);
        if (slot < 0 && tf->full)
            return 0;
        *entry = (slot < 0) ? SG_MISSING : (uint16_t)(slot + 1);
    }
    return *entry;
}

const TbSprite *entry_sprite(const SgSystemTypeface *tf, uint16_t entry)
{
    if (entry == 0 || entry == SG_MISSING)
        return NULL;
    return &tf->sprites[entry - 1];
}

SgSystemTypeface *typeface_for_sheet(const TbSpriteSheet *sheet)
{
    for (auto &tf : g_faces)
        if (tf.sheet == sheet)
            return &tf;
    return NULL;
}

std::string sheet_name(const char *data_fname)
{
    std::string s = data_fname ? data_fname : "";
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    const size_t dot = s.rfind('.');
    if (dot != std::string::npos)
        s.resize(dot);
    return to_lower(s);
}

void sheet_loaded(const TbSpriteSheet *sheet, const char *data_fname)
{
    const std::string name = sheet_name(data_fname);
    const auto it = g_renders.find(name);
    if (it == g_renders.end())
        return;

    SgSystemTypeface *tf = typeface_for_sheet(sheet);
    if (tf == NULL)
        tf = typeface_for_sheet(NULL);
    if (tf == NULL) {
        WARNLOG("sysglyphs: more than %d sprite fonts with system glyphs", SG_MAX_TYPEFACES);
        return;
    }
    free_typeface(tf);
    tf->sheet = sheet;
    tf->name = it->first.c_str();
    bool opened = false;
    try {
        opened = open_typeface(tf, it->second);
    } catch (const std::bad_alloc &) {
        ERRORLOG("sysglyphs: out of memory (typeface)");
    }

    if (!opened)
        free_typeface(tf);
}

} // namespace

/******************************************************************************/

extern "C" TbBool LbSysGlyphsInit(void)
{
    std::unique_lock<std::shared_mutex> lock(g_lock);
    try {
        load_config(install_info.lang_id);
    } catch (const std::bad_alloc &) {
        ERRORLOG("sysglyphs: out of memory loading config");
        return false;
    }
    return true;
}

extern "C" void LbSysGlyphsSheetLoaded(const struct TbSpriteSheet *sheet, const char *data_fname)
{
    if (sheet == NULL)
        return;
    std::unique_lock<std::shared_mutex> lock(g_lock);
    try {
        sheet_loaded(sheet, data_fname);
    } catch (const std::bad_alloc &) {
        ERRORLOG("sysglyphs: out of memory attaching \"%s\"", data_fname ? data_fname : "");
    }
}

extern "C" void LbSysGlyphsSheetFreed(const struct TbSpriteSheet *sheet)
{
    if (sheet == NULL)
        return;
    std::unique_lock<std::shared_mutex> lock(g_lock);
    SgSystemTypeface *tf = typeface_for_sheet(sheet);
    if (tf != NULL)
        free_typeface(tf);
}

extern "C" TbBool LbSysGlyphsAttached(const struct TbSpriteSheet *sheet)
{
    if (sheet == NULL)
        return false;
    std::shared_lock<std::shared_mutex> lock(g_lock);
    return typeface_for_sheet(sheet) != NULL;
}

extern "C" const struct TbSprite *LbSysGlyphSprite(const struct TbSpriteSheet *sheet, uint32_t codepoint)
{
    if (sheet == NULL || codepoint < 0x80 || codepoint > SG_MAX_CODEPOINT)
        return NULL;
    {
        std::shared_lock<std::shared_mutex> lock(g_lock);
        const SgSystemTypeface *tf = typeface_for_sheet(sheet);
        if (tf == NULL)
            return NULL;
        const uint16_t entry = find_entry(tf, codepoint);
        if (entry != 0 || tf->full)
            return entry_sprite(tf, entry);
    }

    std::unique_lock<std::shared_mutex> lock(g_lock);
    SgSystemTypeface *tf = typeface_for_sheet(sheet);
    if (tf == NULL)
        return NULL;
    return entry_sprite(tf, add_glyph(tf, codepoint));
}
