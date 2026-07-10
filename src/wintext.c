// wintext.c (part of mintty)
// Copyright 2008-22 Andy Koppe, 2015-2026 Thomas Wolff
// Adapted from code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "winpriv.h"
#include "winsearch.h"
#include "charset.h"  // wcscpy, wcsncat, combiningdouble
#include "config.h"
#include "winimg.h"  // winimgs_paint
#include "tek.h"
#include "child.h"   // child_tty
#include "perf.h"

#include <winnls.h>
#include <usp10.h>  // Uniscribe
#include <stdlib.h>  // atexit


#define dont_debug_bold 1

#define dont_narrow_via_font

enum {
  FONT_NORMAL    = 0x00,
  FONT_BOLD      = 0x01,
  FONT_ITALIC    = 0x02,
  FONT_BOLDITAL  = FONT_BOLD | FONT_ITALIC,
  FONT_UNDERLINE = 0x04,
  FONT_BOLDUND   = FONT_BOLD | FONT_UNDERLINE,
  FONT_STRIKEOUT = 0x08,
  FONT_HIGH      = 0x10,
  FONT_ZOOMFULL  = 0x20,
  FONT_ZOOMSMALL = 0x40,
  FONT_ZOOMDOWN  = 0x80,
  FONT_DIM       = 0x100,
  // keep these last:
  FONT_WIDE      = 0x200,
#ifdef narrow_via_font
#warning narrowing via font is deprecated
  FONT_NARROW    = 0x400,
  FONT_MAXNO     = FONT_WIDE + FONT_NARROW
#else
  FONT_NARROW    = 0,	// disabled narrowing via font
  FONT_MAXNO     = 2 * FONT_WIDE
#endif
};

enum {LDRAW_CHAR_NUM = 31, LDRAW_CHAR_TRIES = 4};

// Possible linedraw character mappings, in order of decreasing suitability.
// The first choice is the same as used by xterm in most cases,
// except the diamond for which the narrower form is more authentic
// (see http://vt100.net/docs/vt220-rm/table2-4.html).
// The last resort for each is an ASCII character, which we assume will be
// available in any font.
static const wchar linedraw_chars[LDRAW_CHAR_NUM][LDRAW_CHAR_TRIES] = {
  {0x2666, 0x25C6, '*'},           // 0x60 '`' Diamond ♦ ◆
  {0x2592, '#'},                   // 0x61 'a' Checkerboard (error)
  {0x2409, 0x2192, 0x01AD, 't'},   // 0x62 'b' Horizontal tab
  {0x240C, 0x21A1, 0x0192, 'f'},   // 0x63 'c' Form feed
  {0x240D, 0x21B5, 0x027C, 'r'},   // 0x64 'd' Carriage return
  {0x240A, 0x21B4, 0x019E, 'n'},   // 0x65 'e' Linefeed
  {0x00B0, 'o'},                   // 0x66 'f' Degree symbol
  {0x00B1, '~'},                   // 0x67 'g' Plus/minus
  {0x2424, 0x21B4, 0x019E, 'n'},   // 0x68 'h' Newline
  {0x240B, 0x2193, 0x028B, 'v'},   // 0x69 'i' Vertical tab
  {0x2518, '+'},                   // 0x6A 'j' Lower-right corner
  {0x2510, '+'},                   // 0x6B 'k' Upper-right corner
  {0x250C, '+'},                   // 0x6C 'l' Upper-left corner
  {0x2514, '+'},                   // 0x6D 'm' Lower-left corner
  {0x253C, '+'},                   // 0x6E 'n' Crossing lines
  {0x23BA, 0x203E, ' '},           // 0x6F 'o' High horizontal line
  {0x23BB, 0x207B, ' '},           // 0x70 'p' Medium-high horizontal line
  {0x2500, 0x2014, '-'},           // 0x71 'q' Middle horizontal line
  {0x23BC, 0x208B, ' '},           // 0x72 'r' Medium-low horizontal line
  {0x23BD, '_'},                   // 0x73 's' Low horizontal line
  {0x251C, '+'},                   // 0x74 't' Left "T"
  {0x2524, '+'},                   // 0x75 'u' Right "T"
  {0x2534, '+'},                   // 0x76 'v' Bottom "T"
  {0x252C, '+'},                   // 0x77 'w' Top "T"
  {0x2502, '|'},                   // 0x78 'x' Vertical bar
  {0x2264, '#'},                   // 0x79 'y' Less than or equal to
  {0x2265, '#'},                   // 0x7A 'z' Greater than or equal to
  {0x03C0, '#'},                   // 0x7B '{' Pi
  {0x2260, '#'},                   // 0x7C '|' Not equal to
  {0x00A3, 'L'},                   // 0x7D '}' UK pound sign
  {0x00B7, '.'},                   // 0x7E '~' Centered dot
};


// colour values; this should perhaps be part of struct term
COLORREF colours[COLOUR_NUM];

// diagnostic information flag
bool show_charinfo = false;


// master font family properties
LOGFONT lfont;
// logical font size, as configured (< 0: pixel size)
int font_size;
// scaled font size; pure font height, without spacing
static int font_height;
// character cell size, including spacing:
int cell_width, cell_height;
// border padding:
int PADDING = 1;
int OFFSET = 0;
// width mode
bool font_ambig_wide;


typedef enum {BOLD_SHADOW, BOLD_FONT} BOLD_MODE;
typedef enum {DIM_DIM, DIM_FONT} DIM_MODE;
typedef enum {UND_LINE, UND_FONT} UND_MODE;

struct charpropcache {
  uint width: 2;
  xchar ch: 21;
} __attribute__((packed));

// font family properties
struct fontfam {
  wstring name;
  wstring name_reported;
  int weight;
  bool isbold;
  char no_rtl;  // 1: no R/Hebrew, 2: no AL/Arabic, 4: either
  HFONT fonts[FONT_MAXNO];
  bool fontflag[FONT_MAXNO];
  bool fontok;
  bool font_dualwidth;
  int width;
  int shift;
  struct charpropcache * cpcache[FONT_BOLDITAL + 1];
  uint cpcachelen[FONT_BOLDITAL + 1];
  bool cached;  // font object cache maintained in dw_has_glyph
  wchar errch;
  int fw_norm;
  int fw_bold;
  BOLD_MODE bold_mode;
  DIM_MODE dim_mode;
  UND_MODE und_mode;
  int row_spacing, col_spacing;
  int descent;
  // VT100 linedraw character mappings for current font:
  wchar win_linedraw_chars[LDRAW_CHAR_NUM];
} fontfamilies[12];  // lengthof(cfg.fontfams) if [11] set via fontfams

int line_scale;

wchar
win_linedraw_char(int i)
{
  int findex = (term.curs.attr.attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;
  struct fontfam * ff = &fontfamilies[findex];
  return ff->win_linedraw_chars[i];
}


char *
fontpropinfo()
{
  //__ Options - Text: font properties information: "Leading": total line padding (see option RowSpacing), Bold/Underline modes (font or manual, see options BoldAsFont/UnderlineManual/UnderlineColour)
  char * fontinfopat = _("Leading: %d, Bold: %s, Underline: %s");
  //__ Options - Text: font properties: value taken from font
  char * fontinfo_font = _("font");
  //__ Options - Text: font properties: value affected by option
  char * fontinfo_manual = _("manual");
  int taglen = max(strlen(fontinfo_font), strlen(fontinfo_manual));
  char * fontinfo = newn(char, strlen(fontinfopat) + 23 + 2 * taglen);
  sprintf(fontinfo, fontinfopat, fontfamilies->row_spacing, 
          fontfamilies->bold_mode ? fontinfo_font : fontinfo_manual,
          fontfamilies->und_mode ? fontinfo_font : fontinfo_manual);
  return fontinfo;
}


uint
colour_dist(colour a, colour b)
{
  return
    2 * sqr(red(a) - red(b)) +
    4 * sqr(green(a) - green(b)) +
    1 * sqr(blue(a) - blue(b));
}

#define dont_debug_brighten

colour
brighten(colour c, colour against, bool monotone)
{
  uint r = red(c), g = green(c), b = blue(c);
  // "brighten" away from the background:
  // if we are closer to black than the contrast reference, rather darken
  bool darken = colour_dist(c, 0) < colour_dist(against, 0);
#ifdef debug_brighten
  printf("%s %06X against %06X\n", darken ? "darkening" : "brighting", c, against);
#endif

  uint _brighter() {
    uint s = min(85, 255 - max(max(r, g), b));
    return make_colour(r + s, g + s, b + s);
  }
  uint _darker() {
    int sub = 70;
    return make_colour(max(0, (int)r - sub), max(0, (int)g - sub), max(0, (int)b - sub));
  }

  colour bright;
  uint thrsh = 22222;  // contrast threshold;
                       // if we're closer to either fg or bg,
                       // turn "brightening" into the other direction

  if (darken) {
    bright = _darker();
#ifdef debug_brighten
    printf("darker %06X -> %06X dist %d\n", c, bright, colour_dist(c, bright));
#endif
    if (colour_dist(bright, c) < thrsh || colour_dist(bright, against) < thrsh) {
      if (monotone) {
        uint r = red(bright), g = green(bright), b = blue(bright);
        return make_colour(r - (r >> 2), g - (g >> 2), b - (b >> 2));
      }
      bright = _brighter();
#ifdef debug_brighten
      printf("   fix %06X -> %06X dist %d/%d\n", c, bright, colour_dist(bright, c), colour_dist(bright, against));
#endif
    }
  }
  else {
    bright = _brighter();
#ifdef debug_brighten
    printf("lightr %06X -> %06X dist %d\n", c, bright, colour_dist(c, bright));
#endif
    if (colour_dist(bright, c) < thrsh || colour_dist(bright, against) < thrsh) {
      if (monotone) {
        uint r = red(bright), g = green(bright), b = blue(bright);
        return make_colour(r + ((256 - r) >> 2), g + ((256 - g) >> 2), b + ((256 - b) >> 2));
      }
      bright = _darker();
#ifdef debug_brighten
      printf("   fix %06X -> %06X dist %d/%d\n", c, bright, colour_dist(bright, c), colour_dist(bright, against));
#endif
    }
  }

  return bright;
}

static uint
get_font_quality(void)
{
  return
    (uchar[]){
      [FS_DEFAULT] = DEFAULT_QUALITY,
      [FS_NONE] = NONANTIALIASED_QUALITY,
      [FS_PARTIAL] = ANTIALIASED_QUALITY,
      [FS_FULL] = CLEARTYPE_QUALITY
    }[(int)cfg.font_smoothing];
}

#define dont_debug_create_font

#define dont_debug_fonts 1

#define dont_debug_win_char_width_init

#if defined(debug_fonts) && debug_fonts > 0
#define trace_font(params)	printf params
#else
#define trace_font(params)	
#endif

static HFONT
create_font(wstring name, int weight, bool underline)
{
#ifdef debug_create_font
  printf("create_font [??]: %d (size %d) 0 w%4d i0 u%d s0\n", font_height, font_size, weight, underline);
#endif
  int height = font_height;
  if (*name == '+') {
    name ++;
    height = height * 16 / 10;
#ifdef debug_create_font
    printf("create_font [??]: %d->%d\n", font_height, height);
#endif
  }
  return
    CreateFontW(
      height, 0, 0, 0, weight, false, underline, false,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      get_font_quality(), FIXED_PITCH | FF_DONTCARE,
      name
    );
}

static int
row_padding(int i, int e)
{
  // may look nicer; used to break box characters; for background discussion,
  // see https://github.com/mintty/mintty/issues/631#issuecomment-279690468
  static bool allow_add_font_padding = true;

  if (i == 0 && e == 0)
    if (allow_add_font_padding)
      return 2;
    else
      return 0;
  else {
    int exc = 0;
    if (i > 3)
      exc = i - 3;
    int adj = e - exc;
    if (allow_add_font_padding || adj <= 0)
      return adj;
    else
      return 0;
  }
}

static char * font_warnings = 0;

static void
font_warning(struct fontfam * ff, char * msg)
{
  // suppress multiple font error messages
  if (ff->name_reported && wcscmp(ff->name_reported, ff->name) == 0) {
    return;
  }
  else {
    if (ff->name_reported)
      delete(ff->name_reported);
    ff->name_reported = wcsdup(ff->name);
  }

  char * fn = cs__wcstoutf(ff->name);
  if (font_warnings) {
    char * newfw = asform("%s\n%s:\n%s", font_warnings, msg, fn);
    free(font_warnings);
    font_warnings = newfw;
  }
  else
    font_warnings = asform("%s:\n%s", msg, fn);
  free(fn);
}

static void
show_font_warnings(void)
{
  if (font_warnings) {
    show_message(font_warnings, MB_ICONWARNING);
    free(font_warnings);
    font_warnings = 0;
  }
}


#ifndef TCI_SRCLOCALE
//old MinGW
#define TCI_SRCLOCALE 0x1000
#endif

#ifdef check_font_ranges
#warning this does not tell us whether a glyph is shown

static GLYPHSET *
win_font_ranges(HDC dc, struct fontfam * ff, int fontno)
{
  if (!ff->fonts[fontno] || fontno >= FONT_BOLDITAL)
    return 0;
  SelectObject(dc, ff->fonts[fontno]);
  int ursize = GetFontUnicodeRanges(dc, 0);
  GLYPHSET * gs = malloc(ursize);
  gs->cbThis = ursize;
  gs->flAccel = 0;
  if (GetFontUnicodeRanges(dc, gs)) {
#ifdef debug_font_ranges
    printf("%d %ls\n", fontno, ff->name);
    for (uint i = 0; i < gs->cRanges; i++) {
      printf("%04X: %d\n", gs->ranges[i].wcLow, gs->ranges[i].cGlyphs);
    }
#endif
  }
  return gs;
}

static bool
glyph_in(WCHAR c, GLYPHSET * gs)
{
  int min = 0;
  int max = gs->cRanges - 1;
  int mid;
  while (max >= min) {
    mid = (min + max) / 2;
    if (c < gs->ranges[mid].wcLow) {
      max = mid - 1;
    } else if (c < gs->ranges[mid].wcLow + gs->ranges[mid].cGlyphs) {
      return true;
    } else {
      min = mid + 1;
    }
  }
  return false;
}

#endif

static UINT
get_default_charset(void)
{
  CHARSETINFO csi;

  long int acp = GetACP();
  int ok = TranslateCharsetInfo((DWORD *)acp, &csi, TCI_SRCCODEPAGE);
  if (ok)
    return csi.ciCharset;
  else
    return DEFAULT_CHARSET;
}

struct data_adjust_font_weights {
  struct fontfam *ff;
  int fw_norm_0, fw_bold_0, fw_norm_1, fw_bold_1, default_charset;
  bool font_found, ansi_found, cs_found;
  bool light_found;
};

static int CALLBACK
enum_fonts_adjust_font_weights(const LOGFONTW * lfp, const TEXTMETRICW * tmp, DWORD fontType, LPARAM lParam)
{
  struct data_adjust_font_weights *data = (struct data_adjust_font_weights *)lParam;
  (void)tmp;
  (void)fontType;

#if defined(debug_fonts) && debug_fonts > 1
  if (!lfp->lfCharSet)
    trace_font(("%ls %dx%d weight %d it %d cs %d %s\n", lfp->lfFaceName, (int)lfp->lfWidth, (int)lfp->lfHeight, (int)lfp->lfWeight, lfp->lfItalic, lfp->lfCharSet, (lfp->lfPitchAndFamily & 3) == FIXED_PITCH ? "fixed" : ""));
#endif

  data->font_found = true;
  if (lfp->lfCharSet == ANSI_CHARSET)
    data->ansi_found = true;
  if (lfp->lfCharSet == data->default_charset || lfp->lfCharSet == DEFAULT_CHARSET)
    data->cs_found = true;

  if (lfp->lfWeight > data->fw_norm_0 && lfp->lfWeight <= data->ff->fw_norm)
    data->fw_norm_0 = lfp->lfWeight;
  if (lfp->lfWeight > data->fw_bold_0 && lfp->lfWeight <= data->ff->fw_bold)
    data->fw_bold_0 = lfp->lfWeight;
  if (lfp->lfWeight < data->fw_norm_1 && lfp->lfWeight >= data->ff->fw_norm)
    data->fw_norm_1 = lfp->lfWeight;
  if (lfp->lfWeight < data->fw_bold_1 && lfp->lfWeight >= data->ff->fw_bold)
    data->fw_bold_1 = lfp->lfWeight;

  return 1;  // continue
}

#define find_light_font_automatically

#ifdef find_light_font_automatically
static int CALLBACK
enum_font_variants(const LOGFONTW * lfp, const TEXTMETRICW * tmp, DWORD fontType, LPARAM lParam)
{
  struct data_adjust_font_weights *data = (struct data_adjust_font_weights *)lParam;
  (void)tmp;
  (void)fontType;

  if (lfp->lfCharSet == data->default_charset || lfp->lfCharSet == DEFAULT_CHARSET) {
    int nlen = wcslen(data->ff->name);
    if (0 == wcsncmp(lfp->lfFaceName, data->ff->name, nlen)
     && (0 == wcscmp(lfp->lfFaceName + nlen + 1, W("Light"))
//     && (wcsstr(lfp->lfFaceName + nlen, W("Light"))
//      || wcsstr(lfp->lfFaceName + nlen, W("Thin"))
        )
       )
    {
#if defined(debug_fonts) && debug_fonts > 1
      trace_font(("<%ls %dx%d weight %d it %d cs %d %s\n", lfp->lfFaceName, (int)lfp->lfWidth, (int)lfp->lfHeight, (int)lfp->lfWeight, lfp->lfItalic, lfp->lfCharSet, (lfp->lfPitchAndFamily & 3) == FIXED_PITCH ? "fixed" : ""));
#endif
      // for later activation in another_font,
      // choose one of Thin, ExtraLight, Ultra Light, Light
      // (maybe according to common preference...) -
      // for now, let's just pick Light if available,
      // and set a flag in data to later set data->ff->dim_mode = DIM_FONT
      data->light_found = true;
    }
  }

  return 1;  // continue
}
#endif

static void
adjust_font_weights(struct fontfam * ff, int findex)
{
  LOGFONTW lf;
  wcscpy(lf.lfFaceName, W(""));
  wcsncat(lf.lfFaceName, ff->name, lengthof(lf.lfFaceName) - 1);
  lf.lfPitchAndFamily = 0;
  //lf.lfCharSet = ANSI_CHARSET;   // report only ANSI character range
  // use this to avoid double error popup (e.g. Font=David):
  lf.lfCharSet = DEFAULT_CHARSET;  // report all supported char ranges

  // find the closest available widths such that
  // fw_norm_0 <= ff->fw_norm <= fw_norm_1
  // fw_bold_0 <= ff->fw_bold <= fw_bold_1
  int default_charset = get_default_charset();
  struct data_adjust_font_weights data = {
    .ff = ff,
    .fw_norm_0 = 0,
    .fw_bold_0 = 0,
    .fw_norm_1 = 1000,
    .fw_bold_1 = 1001,
    .default_charset = default_charset,
    .font_found = false,
    .ansi_found = false,
    .light_found = false,
    .cs_found = default_charset == DEFAULT_CHARSET
  };

  // do not enumerate all fonts for unspecified alternative font
  if (ff->name[0] == 0) {
    ff->fw_norm = 400;
    ff->fw_bold = 700;
    trace_font(("--\n"));
    return;
  }

  HDC dc = GetDC(0);
  EnumFontFamiliesExW(dc, &lf, enum_fonts_adjust_font_weights, (LPARAM)&data, 0);
  trace_font(("font width (%d)%d(%d)/(%d)%d(%d)", data.fw_norm_0, ff->fw_norm, data.fw_norm_1, data.fw_bold_0, ff->fw_bold, data.fw_bold_1));
#ifdef find_light_font_automatically
  if (cfg.dim_as_font) {
    trace_font(("\n"));
    lf.lfFaceName[0] = 0;  // clear font family name
    EnumFontFamiliesExW(dc, &lf, enum_font_variants, (LPARAM)&data, 0);
  }
#endif
  ReleaseDC(0, dc);

  // check if no font found
  if (!data.font_found) {
    font_warning(ff, _("Font not found, using system substitute"));
    ff->fw_norm = 400;
    ff->fw_bold = 700;
    trace_font(("//\n"));
    return;
  }
  if (!data.ansi_found && !data.cs_found) {
    string l;
    if (!strcmp(cfg.charset, "CP437") || ((l = getlocenvcat("LC_CTYPE")) && strstr(l, "CP437"))) {
      // accept limited range
    }
    else if (findex) {
      // don't report for alternative / secondary fonts
    }
    else
      font_warning(ff, _("Font has limited support for character ranges"));
  }

  // set dim mode usage of Light font variation if it exists and shall be used
  if (data.light_found && cfg.dim_as_font)
    ff->dim_mode = DIM_FONT;

  // find available widths closest to selected widths
  if (abs(ff->fw_norm - data.fw_norm_0) <= abs(ff->fw_norm - data.fw_norm_1) && data.fw_norm_0 > 0)
    ff->fw_norm = data.fw_norm_0;
  else if (data.fw_norm_1 < 1000)
    ff->fw_norm = data.fw_norm_1;
  if (abs(ff->fw_bold - data.fw_bold_0) < abs(ff->fw_bold - data.fw_bold_1) || data.fw_bold_1 > 1000)
    ff->fw_bold = data.fw_bold_0;
  else if (data.fw_bold_1 < 1001)
    ff->fw_bold = data.fw_bold_1;
  // ensure bold is bolder than normal
  if (ff->fw_bold <= ff->fw_norm) {
    trace_font((" -> %d/%d", ff->fw_norm, ff->fw_bold));
    if (data.fw_norm_0 < ff->fw_norm && data.fw_norm_0 > 0)
      ff->fw_norm = data.fw_norm_0;
    if (ff->fw_bold - ff->fw_norm < 300) {
      if (data.fw_bold_1 > ff->fw_bold && data.fw_bold_1 < 1001)
        ff->fw_bold = data.fw_bold_1;
      else
        ff->fw_bold = min(ff->fw_norm + 300, 1000);
    }
  }
  // enforce preselected boldness
  int selweight = ff->weight;
  if (selweight < 700 && ff->isbold)
    selweight = 700;
  if (selweight - ff->fw_norm >= 300) {
    trace_font((" -> %d(%d)/%d", ff->fw_norm, selweight, ff->fw_bold));
    ff->fw_norm = selweight;
    ff->fw_bold = min(ff->fw_norm + 300, 1000);
  }
  trace_font((" -> %d/%d\n", ff->fw_norm, ff->fw_bold));
}

static int fonts_found;

static int CALLBACK
enum_fonts_check_font(const LOGFONTW * lfp, const TEXTMETRICW * tmp, DWORD fontType, LPARAM lParam)
{
  (void)lfp, (void)tmp, (void)fontType, (void)lParam;
  fonts_found ++;
  return 1;  // continue
}

static int
check_font(HDC dc, struct fontfam * ff)
{
  LOGFONTW lf;
  wcscpy(lf.lfFaceName, W(""));
  wcsncat(lf.lfFaceName, ff->name, lengthof(lf.lfFaceName) - 1);
  lf.lfPitchAndFamily = 0;
  lf.lfCharSet = DEFAULT_CHARSET;  // report all supported char ranges

  fonts_found = 0;
  EnumFontFamiliesExW(dc, &lf, enum_fonts_check_font, 0, 0);
  return fonts_found;
}

/*
 * Memoisation of win_char_width results (see win_char_width below).
 * win_char_width is invoked from the paint loop (term_paint) for every
 * non-ASCII character cell whose contents changed since the previous
 * frame, i.e. for whole screens of cells when scrolling non-ASCII text.
 * Its uncached implementation costs at least one GetDC/SelectObject/
 * GetCharWidth32W/ReleaseDC round-trip per call, and for symbol ranges
 * it renders the glyph into a bitmap and reads the pixels back
 * (act_char_width), so repeated calls dominate frame time on large
 * (e.g. 4K) windows. Results only depend on the character, the font
 * family/style derived from the attributes, and the current font
 * instances/metrics, so they can be cached until fonts are
 * (re)initialised (win_init_fontfamily), which covers font selection,
 * zooming and DPI changes.
 * The cache is a fixed-size open-addressing hash table; it is a cache,
 * not a map: on collision overflow an old entry is simply evicted.
 */
#define WCW_CACHE_BITS 13
#define WCW_CACHE_SIZE (1 << WCW_CACHE_BITS)
#define WCW_CACHE_PROBES 8

struct wcw_entry {
  uint key;  // wcw_key() result; 0 marks an empty slot
  int wid;   // memoised win_char_width result
};

static struct wcw_entry wcw_cache[WCW_CACHE_SIZE];

/*
 * Build the cache key for character c (a Unicode code point <= 0x10FFFF)
 * under character attributes attr. The key combines everything the
 * uncached function derives from its arguments: the code point, the
 * font family index (clamped as in win_char_width), and the bold/italic
 * style bits as resolved by font4(). Returns a non-zero 27-bit key
 * (c + 1 keeps 0 available as the empty-slot marker).
 */
static uint
wcw_key(xchar c, cattrflags attr)
{
  uint findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10) {
    findex = 0;  // same clamping as in win_char_width
  }
  struct fontfam * ff = &fontfamilies[findex];
  uint bold = ((ff->bold_mode == BOLD_FONT) && (attr & ATTR_BOLD)) ? 1 : 0;
  uint ital = (attr & ATTR_ITALIC)                                 ? 1 : 0;
  return (c + 1) | findex << 21 | bold << 25 | ital << 26;
}

/*
 * Map a cache key to its home slot (Fibonacci hashing).
 * Returns a slot index in [0, WCW_CACHE_SIZE).
 */
static uint
wcw_slot(uint key)
{
  return (key * 2654435761u) >> (32 - WCW_CACHE_BITS);
}

/*
 * Look up key in the cache. On a hit, store the memoised width into
 * *wid and return true; return false on a miss. *wid is unchanged on
 * a miss.
 */
static bool
wcw_lookup(uint key, int * wid)
{
  uint slot = wcw_slot(key);
  for (uint i = 0; i < WCW_CACHE_PROBES; i++) {
    struct wcw_entry * e = &wcw_cache[(slot + i) & (WCW_CACHE_SIZE - 1)];
    if (e->key == key) {
      *wid = e->wid;
      return true;
    }
    if (!e->key) {
      return false;
    }
  }
  return false;
}

/*
 * Store the width result wid for cache key key, reusing an empty or
 * matching slot within the probe window, else evicting the home slot.
 */
static void
wcw_store(uint key, int wid)
{
  uint slot   = wcw_slot(key);
  uint victim = slot;
  for (uint i = 0; i < WCW_CACHE_PROBES; i++) {
    struct wcw_entry * e = &wcw_cache[(slot + i) & (WCW_CACHE_SIZE - 1)];
    if (!e->key || e->key == key) {
      victim = (slot + i) & (WCW_CACHE_SIZE - 1);
      break;
    }
  }
  wcw_cache[victim] = (struct wcw_entry){.key = key, .wid = wid};
}

/*
 * Drop all memoised widths; to be called whenever fonts or font metrics
 * may have changed (font re-initialisation).
 */
static void
wcw_flush(void)
{
  memset(wcw_cache, 0, sizeof wcw_cache);
}

static HFONT   paint_buf_font = 0;      // font selected in paint_buf_dc
static bool    paint_buf_font_ok = false;  // paint_buf_font is trustworthy

/* Forget the cached font selection (buffer drop, font recreation). */
static void
win_select_font_reset(void)
{
  paint_buf_font    = 0;
  paint_buf_font_ok = false;
}

/*
 * Initialise all the fonts of a font family we will need initially:
   Normal (the ordinary font), and optionally bold and underline;
   Other font variations are done if/when they are needed (another_font).

   We also:
   - check the font width and height, correcting our guesses if necessary.
   - verify that the bold font is the same width as the ordinary one, 
     and engage shadow bolding if not.
   - verify that the underlined font is the same width as the ordinary one, 
     and engage manual underlining if not.
 */
static void
win_init_fontfamily(HDC dc, int findex)
{
  struct fontfam * ff = &fontfamilies[findex];
  //printf("fontfamily %d <%ls>\n", findex, ff->name);

  trace_resize(("--- init_fontfamily\n"));

  for (uint i = 0; i < FONT_BOLDITAL; i++) {
    if (ff->fonts[i] && ff->cpcache[i])
      delete(ff->cpcache[i]);
    ff->cpcache[i] = 0;
    ff->cpcachelen[i] = 0;
  }
  // fonts and metrics are about to change; memoised widths go stale
  wcw_flush();
  // font handles are about to be deleted and may be reallocated at the
  // same values; forget the cached selection
  win_select_font_reset();
  ff->cached = false;
  for (uint i = 0; i < FONT_MAXNO; i++) {
    if (ff->fonts[i]) {
      DeleteObject(ff->fonts[i]);
      ff->fonts[i] = 0;
    }
    ff->fontflag[i] = false;
  }

  ff->errch = 0;

  // if initialized as BOLD_SHADOW then real bold is never attempted
  ff->bold_mode = BOLD_FONT;

  // dim attribute implemented as dimmed colour by default
  ff->dim_mode = DIM_DIM;

  ff->und_mode = UND_FONT;
  if (cfg.underl_manual || cfg.underl_colour != (colour)-1)
    ff->und_mode = UND_LINE;

  if (ff->weight) {
    ff->fw_norm = ff->weight;
    ff->fw_bold = min(ff->fw_norm + 300, 1000);
    // adjust selected font weights to available font weights
    trace_font(("-> Weight %d/%d\n", ff->fw_norm, ff->fw_bold));
    adjust_font_weights(ff, findex);
    trace_font(("->     -> %d/%d\n", ff->fw_norm, ff->fw_bold));
  }
  else if (ff->isbold) {
    ff->fw_norm = FW_BOLD;
    ff->fw_bold = FW_HEAVY;
    trace_font(("-> IsBold %d/%d\n", ff->fw_norm, ff->fw_bold));
  }
  else {
    ff->fw_norm = FW_DONTCARE;
    ff->fw_bold = FW_BOLD;
    trace_font(("-> normal %d/%d\n", ff->fw_norm, ff->fw_bold));
  }

  ff->fonts[FONT_NORMAL] = create_font(ff->name, ff->fw_norm, false);
  // as this does not report error and font fallback, check explicitly:
  ff->fontok = check_font(dc, ff);

  LOGFONT logfont;
  GetObject(ff->fonts[FONT_NORMAL], sizeof(LOGFONT), &logfont);
  trace_font(("created font %s %d it %d cs %d\n", logfont.lfFaceName, (int)logfont.lfWeight, logfont.lfItalic, logfont.lfCharSet));
  SelectObject(dc, ff->fonts[FONT_NORMAL]);

  TEXTMETRIC tm;
  int tmok = GetTextMetrics(dc, &tm);
  //printf("TextMetric[%d] %d h %d a %d d %d e %d i %d w %d cs %d <%ls>\n", findex, tmok, tm.tmHeight, tm.tmAscent, tm.tmDescent, tm.tmExternalLeading, tm.tmInternalLeading, tm.tmAveCharWidth, tm.tmCharSet, ff->name);
  if (!tmok || !tm.tmHeight) {
    // corrupt font installation (e.g. deleted font file)
    font_warning(ff, _("Font installation corrupt, using system substitute"));
    wstrset(&ff->name, W(""));
    ff->fonts[FONT_NORMAL] = create_font(ff->name, ff->fw_norm, false);
    GetObject(ff->fonts[FONT_NORMAL], sizeof(LOGFONT), &logfont);
    SelectObject(dc, ff->fonts[FONT_NORMAL]);
    GetTextMetrics(dc, &tm);
  }
  // fix broken font metrics
  if (tm.tmAveCharWidth < 0)
    // Panic Sans reports negative char width
    tm.tmAveCharWidth = - tm.tmAveCharWidth;

  // set average glyph width and optional horizontal shift (for CJK centering)
  ff->width = tm.tmAveCharWidth;
  ff->shift = 0;
  if (findex) {  // #1313
    // note CJK width gap (to double-width), triggered by script attribute
    int shift = (2 * fontfamilies[0].width - tm.tmAveCharWidth) / 2;
    if (shift > 0)
      ff->shift = shift;
  }

#ifdef auto_detect_glyph_shift
  // check font for "narrow" CJK characters (#1312);
  // problem with this approach:
  // a font may contain some CJK ranges but not others,
  // so a script-based approach provides finer-grained distinction;
  // this is now implemented based on configuration setting FontChoice
  int cwide = 0; int cnorm = 0;
  if (*ff->name) {
    int len = GetFontUnicodeRanges(dc, 0);
    GLYPHSET * gs = malloc(len);
    gs->cbThis = len;
    gs->flAccel = 0;
    len = GetFontUnicodeRanges(dc, gs);
    for (uint i = 0; len && i < gs->cRanges; i++) {
      if (is_wide(gs->ranges[i].wcLow))
        cwide += gs->ranges[i].cGlyphs;
      else
        cnorm += gs->ranges[i].cGlyphs;
      if (gs->ranges[i].cGlyphs > 9)
        printf("   %04X/%d <%lc>\n", gs->ranges[i].wcLow, gs->ranges[i].cGlyphs, gs->ranges[i].wcLow);
    }
    free(gs);
  }
  printf("   wide %d norm %d\n", cwide, cnorm);
#endif

  if (!findex)
    lfont = logfont;

#ifdef check_charset_only_for_returned_font
  int default_charset = get_default_charset();
  if (tm.tmCharSet != default_charset && default_charset != DEFAULT_CHARSET) {
    font_warning(ff, _("Font does not support system locale"));
  }
#endif

  float latin_char_width, greek_char_width, line_char_width, cjk_char_width;
  GetCharWidthFloatW(dc, 0x0041, 0x0041, &latin_char_width);
  GetCharWidthFloatW(dc, 0x03B1, 0x03B1, &greek_char_width);
  GetCharWidthFloatW(dc, 0x2500, 0x2500, &line_char_width);
  GetCharWidthFloatW(dc, 0x4E00, 0x4E00, &cjk_char_width);

  // avoid trouble with non-text font (#777, Noto Sans Symbols2)
  if (!latin_char_width) {
    //GetCharWidthFloatW(dc, 0x0020, 0x0020, &latin_char_width);
    latin_char_width = (float)font_size / 16;
  }

  if (!findex) {
    ff->row_spacing = 0;
    if (cfg.auto_leading == 1) {
      //?int ilead = tm.tmInternalLeading - (dpi - 96) / 48;
      int idpi = dpi;  // avoid coercion of tm.tmInternalLeading to unsigned
      int ilead = tm.tmInternalLeading * 96 / idpi;
      ff->row_spacing = row_padding(ilead, tm.tmExternalLeading);
      //printf("row_sp dpi %d int %d -> ild %d (ext %d) -> pad %d + cfg %d\n", dpi, (int)tm.tmInternalLeading, ilead, (int)tm.tmExternalLeading, ff->row_spacing, cfg.row_spacing);
      trace_font(("00 height %d avwidth %d asc %d dsc %d intlead %d extlead %d %ls\n", 
                 (int)tm.tmHeight, (int)tm.tmAveCharWidth, (int)tm.tmAscent, (int)tm.tmDescent, 
                 (int)tm.tmInternalLeading, (int)tm.tmExternalLeading, 
                 ff->name));
    }
    else if (cfg.auto_leading == 2) {
      /*
      	–	tmIntLeading	|	|
      	M			|ascent	| tmHeight
      	Mg			|	|
      	 g	tmDescent		|
      	–	tmExtLeading
      */
      if (tm.tmInternalLeading < 0)
        ff->row_spacing += 2 - tm.tmInternalLeading / 4;
      else if (tm.tmInternalLeading < 2)
        ff->row_spacing += 2 - tm.tmInternalLeading;
      else if (tm.tmInternalLeading > 7)
        ff->row_spacing -= tm.tmExternalLeading;
      trace_font(("vert geom: (int %d) asc %d + dsc %d -> hei %d, + ext %d; -> spc %d %ls\n", 
                (int)tm.tmInternalLeading, (int)tm.tmAscent, (int)tm.tmDescent,
                (int)tm.tmHeight, (int)tm.tmExternalLeading, 
                ff->row_spacing,
                ff->name));
    }

    ff->row_spacing += cfg.row_spacing;
    if (ff->row_spacing < -tm.tmDescent)
      ff->row_spacing = -tm.tmDescent;
    //printf("row_sp %d\n", ff->row_spacing);
    trace_font(("row spacing int %d ext %d -> %+d; add %+d -> %+d; desc %d -> %+d %ls\n", 
        (int)tm.tmInternalLeading, (int)tm.tmExternalLeading, row_padding(tm.tmInternalLeading, tm.tmExternalLeading),
        cfg.row_spacing, row_padding(tm.tmInternalLeading, tm.tmExternalLeading) + cfg.row_spacing,
        (int)tm.tmDescent, ff->row_spacing, ff->name));
    ff->col_spacing = cfg.col_spacing;

    cell_height = tm.tmHeight + ff->row_spacing;
    //cell_width = tm.tmAveCharWidth + ff->col_spacing;
    cell_width = (int)(latin_char_width * 16) + ff->col_spacing;

    line_scale = cell_height * 100 / abs(font_height);

    PADDING = tm.tmAveCharWidth;
    if (cfg.padding >= 0 && cfg.padding < PADDING)
      PADDING = cfg.padding;
  }
  else {
    ff->row_spacing = cell_height - tm.tmHeight;
    //ff->col_spacing = cell_width - tm.tmAveCharWidth;
    ff->col_spacing = cell_width - (int)(latin_char_width * 16);
  }

#ifdef debug_create_font
  printf("init_font_family: size %d -> height %d -> height %d\n", font_size, font_height, cell_height);
#endif

  //ff->font_dualwidth = (tm.tmMaxCharWidth >= tm.tmAveCharWidth * 3 / 2);
  ff->font_dualwidth = (cjk_char_width >= latin_char_width * 3 / 2);

  // Determine whether ambiguous-width characters are wide in this font */
  if (!findex)
    font_ambig_wide =
      greek_char_width >= latin_char_width * 1.5 ||
      line_char_width  >= latin_char_width * 1.5;

#ifdef debug_win_char_width_init
  int w_latin = win_char_width(0x0041, 0);
  int w_greek = win_char_width(0x03B1, 0);
  int w_lines = win_char_width(0x2500, 0);
  printf("%04X %5.2f %d\n", 0x0041, latin_char_width, w_latin);
  printf("%04X %5.2f %d\n", 0x03B1, greek_char_width, w_greek);
  printf("%04X %5.2f %d\n", 0x2500, line_char_width, w_lines);
  bool faw = w_greek > w_latin || w_lines > w_latin;
  printf("font faw %d (dual %d [ambig %d])\n", faw, ff->font_dualwidth, font_ambig_wide);
#endif

  // See what RTL glyphs are available.
  ushort rtlglyphs[2];
  GetGlyphIndicesW(dc, W("אا"), 2, rtlglyphs, true);
  ff->no_rtl = (rtlglyphs[0] == 0xFFFF) | (rtlglyphs[1] == 0xFFFF) << 1;
  if (ff->no_rtl)
    ff->no_rtl |= 4;
  //printf("RTL glyphs %04X %04X %d\n", rtlglyphs[0], rtlglyphs[1], ff->no_rtl);

  // Initialise VT100 linedraw character mappings.
  // See what glyphs are available.
  ushort glyphs[LDRAW_CHAR_NUM][LDRAW_CHAR_TRIES];
  GetGlyphIndicesW(dc, *linedraw_chars, LDRAW_CHAR_NUM * LDRAW_CHAR_TRIES,
                   *glyphs, true);

  // For each character, try the list of possible mappings until either we
  // find one that has a glyph in the font or we hit the ASCII fallback.
  for (uint i = 0; i < LDRAW_CHAR_NUM; i++) {
    bool decbox = 'j' <= i + '`' && i + '`' <= 'x';
    uint j = 0;
    while (linedraw_chars[i][j] >= 0x80 &&
           !decbox &&  // no substitutes for self-drawn box graphics
           (glyphs[i][j] == 0xFFFF || glyphs[i][j] == 0x1F))
      j++;
    ff->win_linedraw_chars[i] = linedraw_chars[i][j];
  }

  ff->fonts[FONT_UNDERLINE] = create_font(ff->name, ff->fw_norm, true);

 /*
  * Some fonts, e.g. 9-pt Courier, draw their underlines
  * outside their character cell. We successfully prevent
  * screen corruption by clipping the text output, but then
  * we lose the underline completely. Here we try to work
  * out whether this is such a font, and if it is, we set a
  * flag that causes underlines to be drawn by hand.
  *
  * Having tried other more sophisticated approaches (such
  * as examining the TEXTMETRIC structure or requesting the
  * height of a string), I think we'll do this the brute
  * force way: we create a small bitmap, draw an underlined
  * space on it, and test to see whether any pixels are
  * foreground-coloured. (Since we expect the underline to
  * go all the way across the character cell, we only search
  * down a single column of the bitmap, half way across.)
  */
  if (ff->und_mode == UND_FONT) {
    HDC und_dc = CreateCompatibleDC(dc);
    HBITMAP und_bm = CreateCompatibleBitmap(dc, cell_width, cell_height);
    HBITMAP und_oldbm = SelectObject(und_dc, und_bm);
    SelectObject(und_dc, ff->fonts[FONT_UNDERLINE]);
    SetTextAlign(und_dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    SetTextColor(und_dc, RGB(255, 255, 255));
    SetBkColor(und_dc, RGB(0, 0, 0));
    SetBkMode(und_dc, OPAQUE);
    ExtTextOutA(und_dc, 0, 0, ETO_OPAQUE, null, " ", 1, null);

    bool gotit = false;
    // look for font-generated underline in character cell
    //int i = 0;
    // look for font-generated underline in descender section only
    int i = tm.tmAscent;
    //int i = tm.tmAscent + 1;
    for (; i < cell_height; i++) {
      COLORREF c = GetPixel(und_dc, cell_width / 2, i);
      if (c != RGB(0, 0, 0))
        gotit = true;
    }
    SelectObject(und_dc, und_oldbm);
    DeleteObject(und_bm);
    DeleteDC(und_dc);
    if (!gotit) {
      trace_font(("ul outbox %ls\n", ff->name));
      ff->und_mode = UND_LINE;
      DeleteObject(ff->fonts[FONT_UNDERLINE]);
      ff->fonts[FONT_UNDERLINE] = 0;
    }
  }

  if (ff->bold_mode == BOLD_FONT)
    ff->fonts[FONT_BOLD] = create_font(ff->name, ff->fw_bold, false);

  ff->descent = tm.tmAscent + 1;
  if (ff->descent >= cell_height)
    ff->descent = cell_height - 1;

  int fontsize[FONT_UNDERLINE + 1];
#ifdef handle_baseline_leap
  int base_ascent = tm.tmAscent;
#endif
  for (uint i = 0; i < lengthof(fontsize); i++) { // could skip FONT_ITALIC here
    if (ff->fonts[i]) {
      if (SelectObject(dc, ff->fonts[i]) && GetTextMetrics(dc, &tm)) {
        fontsize[i] = tm.tmAveCharWidth + 256 * tm.tmHeight;
        trace_font(("%02X height %d avwidth %d asc %d dsc %d intlead %d extlead %d %ls\n", 
                    i, (int)tm.tmHeight, (int)tm.tmAveCharWidth, 
                    (int)tm.tmAscent, (int)tm.tmDescent, 
                    (int)tm.tmInternalLeading, (int)tm.tmExternalLeading, 
                    ff->name));
#ifdef handle_baseline_leap
        if (i == FONT_BOLD && tm.tmAscent < base_ascent) {
          // for Courier New, this correlates with a significant visual leap 
          // of the bold font from the baseline of the normal font,
          // but not for other fonts; so let's do nothing
        }
#endif
      }
      else
        fontsize[i] = -i;
    }
    else
      fontsize[i] = -i;
  }

  if (fontsize[FONT_UNDERLINE] != fontsize[FONT_NORMAL]) {
    trace_font(("ul size!= %ls\n", ff->name));
    ff->und_mode = UND_LINE;
    DeleteObject(ff->fonts[FONT_UNDERLINE]);
    ff->fonts[FONT_UNDERLINE] = 0;
  }

  if (ff->bold_mode == BOLD_FONT) {
    int diffsize = abs(fontsize[FONT_BOLD] - fontsize[FONT_NORMAL]);
#if defined(debug_create_font) || defined(debug_bold) || defined(debug_size)
    if (*ff->name)
      printf("bold_mode %d font_size %d size %d bold %d diff %d %s %ls\n",
             ff->bold_mode, font_size,
             fontsize[FONT_NORMAL], fontsize[FONT_BOLD], diffsize,
             fontsize[FONT_BOLD] != fontsize[FONT_NORMAL] ? "=/=" : "===",
             ff->name);
#endif
    if (diffsize * 16 > fontsize[FONT_NORMAL]) {
      trace_font(("bold_mode %d\n", ff->bold_mode));
      ff->bold_mode = BOLD_SHADOW;
      DeleteObject(ff->fonts[FONT_BOLD]);
      ff->fonts[FONT_BOLD] = 0;
    }
  }

  trace_font(("bold_mode %d\n", ff->bold_mode));
  ff->fontflag[FONT_NORMAL] = true;
  ff->fontflag[FONT_BOLD] = true;
  ff->fontflag[FONT_UNDERLINE] = true;
}

static wstring
wcscasestr(wstring in, wstring find)
{
#if CYGWIN_VERSION_API_MINOR < 206
#define wcsncasecmp wcsncmp
#endif
  int l = wcslen(find);
  wstring look = in;
  for (int i = 0; i <= (int)wcslen(in) - l; i++, look++) { // uint fails!
    if (0 == wcsncasecmp(look, find, l)) {
      return look;
    }
  }
  return 0;
}

static int CALLBACK
enum_fonts_find_Fraktur(const LOGFONTW * lfp, const TEXTMETRICW * tmp, DWORD fontType, LPARAM lParam)
{
  (void)tmp;
  (void)fontType;
  wstring * fnp = (wstring *)lParam;

#if defined(debug_fonts) && debug_fonts > 2
  trace_font(("%ls %dx%d %d it %d cs %d %s\n", lfp->lfFaceName, (int)lfp->lfWidth, (int)lfp->lfHeight, (int)lfp->lfWeight, lfp->lfItalic, lfp->lfCharSet, (lfp->lfPitchAndFamily & 3) == FIXED_PITCH ? "fixed" : ""));
#endif
  if ((lfp->lfPitchAndFamily & 3) == FIXED_PITCH
   && !lfp->lfCharSet
   && lfp->lfFaceName[0] != '@'
     )
  {
    if (wcscasestr(lfp->lfFaceName, W("Fraktur"))) {
      *fnp = wcsdup(lfp->lfFaceName);
      return 0;  // done
    }
    else if (wcscasestr(lfp->lfFaceName, W("Blackletter"))) {
      *fnp = wcsdup(lfp->lfFaceName);
      // continue to look for "Fraktur"
    }
  }
  return 1;  // continue
}

void
findFraktur(wstring * fnp)
{
  LOGFONTW lf;
  wcscpy(lf.lfFaceName, W(""));
  lf.lfPitchAndFamily = 0;
  lf.lfCharSet = ANSI_CHARSET;   // report only ANSI character range

  HDC dc = GetDC(0);
  EnumFontFamiliesExW(dc, 0, enum_fonts_find_Fraktur, (LPARAM)fnp, 0);
  ReleaseDC(0, dc);
}


/*
 * Initialize fonts for all configured font families.
 */
void
win_init_fonts(int size, bool allfonts)
{
  trace_resize(("--- init_fonts %d\n", size));

  HDC dc = GetDC(wnd);

  font_size = size;
#ifdef debug_dpi
  printf("dpi %d dev %d\n", dpi, GetDeviceCaps(dc, LOGPIXELSY));
#endif
  if (cfg.handle_dpichanged && per_monitor_dpi_aware)
    font_height =
      font_size > 0 ? -MulDiv(font_size, dpi, 72) : -font_size;
      // dpi is determined initially and via WM_WINDOWPOSCHANGED;
      // if WM_DPICHANGED were used, this would need to be modified
  else
    font_height =
      font_size > 0 ? -MulDiv(font_size, GetDeviceCaps(dc, LOGPIXELSY), 72) : -font_size;

  static bool initinit = true;
  for (uint fi = 0; fi < lengthof(fontfamilies); fi++) {
    // for pre-initialisation of font geometry, skip alternative fonts
    if (!allfonts && fi)
      break;

    if (!fi) {
      fontfamilies[fi].name = cfg.font.name;
      fontfamilies[fi].weight = cfg.font.weight;
      fontfamilies[fi].isbold = cfg.font.isbold;
    }
#ifdef set_RTL_fallback_font_hard_coded
    else if (fi == 11) {
      fontfamilies[fi].name = W("Courier New");
      fontfamilies[fi].weight = 400;
      fontfamilies[fi].isbold = false;
    }
#endif
    else {
      fontfamilies[fi].name = cfg.fontfams[fi].name;
      fontfamilies[fi].weight = cfg.fontfams[fi].weight;
      fontfamilies[fi].isbold = false;
    }
    if (fi == 20 - 10 && !*(fontfamilies[fi].name))
      findFraktur(&fontfamilies[fi].name);
    if (initinit)
      fontfamilies[fi].name_reported = null;

    win_init_fontfamily(dc, fi);
  }
  if (initinit)
    show_font_warnings();
  initinit = false;

  ReleaseDC(wnd, dc);
}

wstring
win_get_font(uint fi)
{
  if (fi < lengthof(fontfamilies))
    if (fontfamilies[fi].fontok) {
#ifdef filter_controls_from_fontname
      wstring fn = fontfamilies[fi].name;
      while (*fn) {
        if (*fn < ' ' || *fn == '\177') {
          // most unlikely to happen if fontok is true
          return W("??");
        }
        fn++;
      }
#endif
      return fontfamilies[fi].name;
    }
    else
      return W("?");
  else
    return null;
}

void
win_change_font(uint fi, wstring fn)
{
  if (fi < lengthof(fontfamilies)) {
    fontfamilies[fi].name = fn;
    fontfamilies[fi].name_reported = null;
    HDC dc = GetDC(wnd);
    win_init_fontfamily(dc, fi);
    ReleaseDC(wnd, dc);
    win_adapt_term_size(true, false);
    win_font_cs_reconfig(true);
  }
}

uint
win_get_font_size(void)
{
  return abs(font_size);
}

void
win_set_font_size(int size, bool sync_size_with_font)
{
  trace_resize(("--- win_set_font_size %d %d×%d\n", size, term.rows, term.cols));
  size = size ? sgn(font_size) * min(size, 72) : cfg.font.size;
  if (size != font_size) {
    win_init_fonts(size, true);
    trace_resize((" (win_set_font_size -> win_adapt_term_size)\n"));
    win_adapt_term_size(sync_size_with_font, false);
  }
}

void
win_zoom_font(int zoom, bool sync_size_with_font)
{
  trace_resize(("--- win_zoom_font %d\n", zoom));
  win_set_font_size(zoom ? max(1, abs(font_size) + zoom) : 0, sync_size_with_font);
}


static HDC dc;
static enum { UPDATE_IDLE, UPDATE_BLOCKED, UPDATE_PENDING } update_state;
static bool ime_open = false;

/*
 * Display back buffer (option DisplayBuffering).
 * Painting a display update directly to the window DC takes long enough
 * on large windows that the compositor shows partially painted frames
 * (tearing/shearing during scrolling output). When buffering is
 * engaged, term_paint output via the global dc is routed into this
 * persistent memory bitmap instead and transferred to the window with
 * a single BitBlt of the text area per update.
 * The buffer accumulates the same incrementally painted state as the
 * displines cache; whenever a frame is painted directly instead
 * (buffering off or bypassed), the buffer goes stale relative to the
 * displines cache, which paint_buf_stale records so that the next
 * buffered frame repaints everything into the buffer first.
 */
static HDC     paint_buf_dc = 0;        // memory DC of the back buffer
static HBITMAP paint_buf_bm = 0;        // bitmap selected into paint_buf_dc
static uint *  paint_buf_bits = 0;      // DIB pixel store (0: plain bitmap)
#ifndef NDEBUG
static HRGN    paint_buf_scratch_rgn = 0;  // scratch for debug clip check
#endif
static int     paint_dc_busy = 0;       // transform/clip nesting depth on dc


/*
 * Track whether a world transform or clip region is active on the
 * global dc. All such state in the paint path is transient and paired
 * (RTL line mirroring, glyph zoom, curly underline clip,
 * self-drawn graphics clip); each pair brackets its section with
 * push/pop. win_fill_rect writes the DIB pixel store directly only at
 * depth 0, else falls back to FillRect which honours the DC state.
 * Debug builds cross-check the counter against the DC (see
 * win_fill_rect), so an unbalanced pair reports itself.
 */
static void
paint_dc_busy_push(void)
{
  paint_dc_busy++;
}

static void
paint_dc_busy_pop(void)
{
  assert(paint_dc_busy > 0);
  if (paint_dc_busy > 0) {
    paint_dc_busy--;
  }
}
static int     paint_buf_w  = 0;        // back buffer width (pixels)
static int     paint_buf_h  = 0;        // back buffer height (pixels)
static bool    paint_buf_stale = true;  // buffer lags the displines cache
static bool    paint_buffered  = false; // painting currently routed to buffer
static HDC     paint_win_dc = 0;        // window DC while routed to buffer

/*
 * Select font f into the global dc, skipping the call when f is
 * already selected. The skip only applies while painting is routed to
 * the private, persistent back buffer DC, whose selected font nobody
 * else changes: all font selection in the paint path goes through this
 * function, the GDI+ emoji path restores the DC state it touches, and
 * measurement helpers use their own DCs. On the shared window DC
 * (unbuffered painting), whose state resets with every GetDC, the
 * selection is always issued. The cache is invalidated when the buffer
 * is dropped and when fonts are recreated (win_init_fontfamily), the
 * latter also guarding against GDI handle reuse after font deletion.
 */
static void
win_select_font(HFONT f)
{
  if (paint_buffered) {
    assert(dc == paint_buf_dc);
    if (paint_buf_font_ok && f == paint_buf_font) {
      return;
    }
    SelectObject(dc, f);
    paint_buf_font    = f;
    paint_buf_font_ok = true;
    return;
  }
  SelectObject(dc, f);
}

static int     paint_dirty_top = 0;     // first character row painted (incl.)
static int     paint_dirty_bot = -1;    // last character row painted (incl.)

static int update_skipped = 0;
int lines_scrolled = 0;

#define dont_debug_cursor 1

static struct charnameentry {
  xchar uc;
  string un;
} * charnametable = null;
static int charnametable_len = 0;
static int charnametable_alloced = 0;
static bool charnametable_init = false;

static void
init_charnametable()
{
  if (charnametable_init)
    return;
  charnametable_init = true;

  void add_charname(uint cc, char * cn) {
    if (charnametable_len >= charnametable_alloced) {
      charnametable_alloced += 999;
      if (!charnametable)
        charnametable = newn(struct charnameentry, charnametable_alloced);
      else
        charnametable = renewn(charnametable, charnametable_alloced);
    }

    charnametable[charnametable_len].uc = cc;
    charnametable[charnametable_len].un = strdup(cn);
    charnametable_len++;
  }

  char * cnfn = get_resource_file(W("info"), W("charnames.txt"), false);
  FILE * cnf = 0;
  if (cnfn) {
    cnf = fopen(cnfn, "r");
    free(cnfn);
  }
  if (cnf) {
    uint cc;
    char cn[100];
    while (fscanf(cnf, "%X %[- A-Z0-9]", &cc, cn) == 2) {
      add_charname(cc, cn);
    }
    fclose(cnf);
  }
  else {
    cnf = fopen("/usr/share/unicode/ucd/UnicodeData.txt", "r");
    if (!cnf)
      return;
    FILE * crf = fopen("/usr/share/unicode/ucd/NameAliases.txt", "r");
    uint ccorr = 0;
    char buf[100];
    char nbuf[100];
    while (fgets(buf, sizeof(buf), cnf)) {
      uint cc;
      char cn[99];
      if (sscanf(buf, "%X;%[- A-Z0-9];", &cc, cn) == 2) {
        //0020;SPACE;Zs;0;WS;;;;;N;;;;;
        if (crf) {
          while (ccorr < cc && fgets(nbuf, sizeof(nbuf), crf)) {
            sscanf(nbuf, "%X;", &ccorr);
          }
          if (ccorr == cc && strstr(nbuf, ";correction")) {
            //2118;WEIERSTRASS ELLIPTIC FUNCTION;correction
            sscanf(nbuf, "%X;%[- A-Z0-9];", &ccorr, cn);
          }
        }
        add_charname(cc, cn);
      }
    }
    fclose(cnf);
    if (crf)
      fclose(crf);
  }
}

static char *
charname(xchar ucs)
{
  // binary search in table
  int min = 0;
  int max = charnametable_len - 1;
  int mid;
  while (max >= min) {
    unsigned long midu;
    unsigned char * mide;
    mid = (min + max) / 2;
    mide = (unsigned char *) charnametable[mid].un;
    midu = charnametable[mid].uc;
    if (midu < ucs) {
      min = mid + 1;
    } else if (midu > ucs) {
      max = mid - 1;
    } else {
      return (char *) mide;
    }
  }
  return "";
}

void
toggle_charinfo()
{
  show_charinfo = !show_charinfo;
}

static char *
get_char_info(termchar * cpoi, bool doret)
{
  init_charnametable();

  static termchar * pp = 0;
  static termchar prev; // = (termchar) {.cc_next = 0, .chr = 0, .attr = CATTR_DEFAULT};
  char * cs = 0;

  // return if base character same as previous and no combining chars
  if (!doret && cpoi == pp && cpoi && cpoi->chr == prev.chr && !cpoi->cc_next)
    return 0;

#define dont_debug_emojis

  if (cpoi && cfg.emojis && (cpoi->attr.attr & TATTR_EMOJI)) {
    if (!doret && cpoi == pp)
      return 0;
    cs = get_emoji_description(cpoi);
#ifdef debug_emojis
    printf("Emoji sequence: %s\n", cs);
#endif
  }

  pp = cpoi;

  if (!cs && cpoi) {
    prev = *cpoi;

    cs = strdup("");

    char * cn = strdup("");

    xchar chbase = 0;
#ifdef show_only_1_charname
    bool combined = false;
#endif
    // show char codes
    while (cpoi) {
      cs = renewn(cs, strlen(cs) + 8 + 1);
      char * cp = &cs[strlen(cs)];
      xchar ci;
      if (is_high_surrogate(cpoi->chr) && cpoi->cc_next && is_low_surrogate((cpoi + cpoi->cc_next)->chr)) {
        ci = combine_surrogates(cpoi->chr, (cpoi + cpoi->cc_next)->chr);
        sprintf(cp, "U+%05X ", ci);
        cpoi += cpoi->cc_next;
      }
      else {
        ci = cpoi->chr;
        sprintf(cp, "U+%04X ", ci);
      }
      if (!chbase)
        chbase = ci;
      char * cni = charname(ci);
      if (cni && *cni) {
        cn = renewn(cn, strlen(cn) + strlen(cni) + 4);
        sprintf(&cn[strlen(cn)], "| %s ", cni);
      }

      if (cpoi->cc_next) {
#ifdef show_only_1_charname
        combined = true;
#endif
        cpoi += cpoi->cc_next;
      }
      else
        cpoi = null;
    }
#ifdef show_only_1_charname
    char * cn = charname(chbase);
    char * extra = combined ? " combined..." : "";
    cs = renewn(cs, strlen(cs) + strlen(cn) + strlen(extra) + 1);
    sprintf(&cs[strlen(cs)], "%s%s", cn, extra);
#else
    cs = renewn(cs, strlen(cs) + strlen(cn) + 1);
    sprintf(&cs[strlen(cs)], "%s", cn);
    free(cn);
#endif
    int n = strlen(cs) - 1;
    if (cs[n] == ' ')
      cs[n] = 0;
  }

  return cs;
}

static void
show_status_line()
{
#if CYGWIN_VERSION_API_MINOR >= 74
  term_cursor curs = term.curs;
  term.st_active = true;
  cattr erase_attr = term.erase_char.attr;

  // get current character from normal screen cursor position
  termline * displine = term.displines[term.curs.y];
  termchar * dispchar = &displine->chars[term.curs.x];

  term.curs.x = 0;
  term.curs.y = term.rows;

  colour bg = win_get_colour(FG_COLOUR_I);
  bg = ((bg & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
  term.curs.attr.attr &= ~(ATTR_FGMASK | ATTR_BGMASK);
  term.curs.attr.attr |= (TRUE_COLOUR << ATTR_FGSHIFT) | (TRUE_COLOUR << ATTR_BGSHIFT);
  term.curs.attr.truefg = win_get_colour(BG_COLOUR_I);
  term.curs.attr.truebg = bg;
  term.erase_char.attr.attr &= ~(ATTR_FGMASK | ATTR_BGMASK);
  term.erase_char.attr.attr |= (TRUE_COLOUR << ATTR_FGSHIFT) | (TRUE_COLOUR << ATTR_BGSHIFT);
  term.erase_char.attr.truefg = win_get_colour(BG_COLOUR_I);
  term.erase_char.attr.truebg = bg;

  bool status_bell = false;
  if (term.bell.last_bell) {
    // flash status line bell 6 times for 2s
    // - let's make that 5s, in order to smooth out chaotic blinking a bit
    // for a better solution, we'd need a timer
    int deltabell = (mtime() - term.bell.last_bell) / (5000 / 11);
    if (deltabell < 11 && !(deltabell & 1))
      status_bell = true;
  }

  if (status_bell) {
    term.curs.utf = true;
    term_update_cs();
  }
  wchar wstbuf[term.cols + 1];

  wchar debug[22];
  *debug = 0;
  if (cfg.status_debug) {
    wchar kblayout[KL_NAMELENGTH];
    GetKeyboardLayoutNameW(kblayout);
    wchar * kbl = kblayout;
    while (*kbl == '0')
      kbl++;
    wcscpy(debug, W(" ["));
    if (cfg.status_debug & 1)
      wcscat(debug, kbl);
    if (cfg.status_debug & 2) {
      wcscat(debug, W("."));
      extern uint mods_debug;
      swprintf(&debug[wcslen(debug)], 9, W("%06X"), mods_debug);
    }
    wcscat(debug, W("]"));
  }

  swprintf(wstbuf, term.cols + 1, W("%s%s%s%s%s%ls %s%s@%02d:%03d%s%s%s%s%s %ls%s"), 
                 term.st_kb_flag ?
                     (term.st_kb_flag == 16 ? "Hex "
                      : term.st_kb_flag == 10 ? "Dec "
                      : term.st_kb_flag == 8 ? "Oct "
                      : term.st_kb_flag == 4 ? "Alt "
                      : term.st_kb_flag == 2 ? "Com "
                      : ""
                     )
                   : "",
                 term.vt220_keys ? "VT220" : "",
                 term.app_cursor_keys ? "↕" : "",
                 term.app_keypad ? "±" : "",
                 child_tty(),
                 debug,
                 term.printing ? "⎙" : "",
                 term.bracketed_paste ? "⁅⁆" : "",
                 curs.y, curs.x,
                 term.on_alt_screen ? "A🖵" : "",
                 term.insert ? "⎀" : "",
                 term.curs.wrapnext ? "↵" : "",
                 term.marg_left || term.marg_right != term.cols - 1
                 || term.marg_top || term.marg_bot != term.rows - 1
                   ? "⬚" : "",
                 term.curs.origin ? "⊡" : "",
                 status_bell ? W("🔔 ") : W(""),   // bell indicator 🔔 or 🛎️ 
                 get_char_info(dispchar, true) ?: ""
                 );
  int n = 0;
  for (wchar * cp = wstbuf; *cp; cp++)
    if (is_high_surrogate(*cp) && is_low_surrogate(cp[1])) {
      write_ucschar(*cp, cp[1], (n += 2, 2));  // simplified width assumptions
      cp++;
    }
    else
      write_char(*cp, (n++, 1));
  for (; n < term.cols; n++)
    write_char(W(' '), 1);

  term.erase_char.attr = erase_attr;
  term.st_active = false;
  term.curs = curs;
  if (status_bell) {
    term_update_cs();
  }
#endif
}

static void
show_curchar_info(char tag)
{
  if (term.st_type == 1)
    show_status_line();

  if (!show_charinfo)
    return;

  (void)tag;

  void show_char_msg(char * cs) {
    static char * prev = null;
    char * _cs = cs ?: "";
    if (!prev || 0 != strcmp(_cs, prev)) {
      //printf("[%c]%s\n", tag, cs);
      if (nonascii(_cs)) {
        wchar * wcs = cs__utftowcs(_cs);
        SetWindowTextW(wnd, wcs);
        free(wcs);
      }
      else
        SetWindowTextA(wnd, _cs);
    }
    if (prev)
      free(prev);
    prev = cs;
  }

  int line = term.curs.y - term.disptop;
  if (line < 0 || line >= term.rows) {
    show_char_msg(0);
  }
  else {
    termline * displine = term.displines[line];
    termchar * dispchar = &displine->chars[term.curs.x];
    char * cs = get_char_info(dispchar, false);
    if (cs)
      show_char_msg(cs);  // does free(cs);
  }
}


/*
 * Whether display updates can currently be routed through the back
 * buffer. Returns false for the modes that paint to the window outside
 * the global dc, which the buffer cannot capture:
 * - Tektronix mode paints via its own window DC;
 * - sixel images are painted directly by winimgs_paint with repainting
 *   suppression, so blitting buffer content over them would erase them;
 * - horizontal view scrolling applies a world transform to the paint
 *   target which does not carry over to a blitted buffer.
 */
static bool
paint_buffer_usable(void)
{
  if (!cfg.display_buffering || tek_mode) {
    if (!cfg.display_buffering)
      PERF_COUNT(buffer_reject_disabled, 1);
    if (tek_mode)
      PERF_COUNT(buffer_reject_tek, 1);
    return false;
  }
  if (horclip() != 0) {
    PERF_COUNT(buffer_reject_horclip, 1);
    return false;
  }
  if (term.imgs.first) {
    PERF_COUNT(buffer_reject_imgs, 1);
    return false;
  }
  return true;
}

/*
 * Release the back buffer and mark it stale.
 * The DC must be deleted before the bitmap: DeleteObject fails on a
 * bitmap that is still selected into a DC (which would leak one
 * client-sized bitmap per call), while DeleteDC implicitly deselects.
 */
static void
paint_buffer_drop(void)
{
  if (paint_buf_dc) {
    DeleteDC(paint_buf_dc);
    paint_buf_dc = 0;
  }
  if (paint_buf_bm) {
    DeleteObject(paint_buf_bm);
    paint_buf_bm = 0;
  }
  paint_buf_bits = 0;
  win_select_font_reset();
  paint_buf_w = 0;
  paint_buf_h = 0;
  paint_buf_stale = true;
}

/*
 * Route subsequent painting via the global dc into the back buffer.
 * The global dc must hold the window target (from GetDC or BeginPaint).
 * Returns true if buffering was engaged; win_paint_buffer_end must then
 * be called after term_paint. Returns false to paint directly to the
 * window as without buffering.
 */
static void selfdraw_cache_maintain(void);

static bool
win_paint_buffer_begin(void)
{
  // reset overflowed drawn-graphics caches at this safe point
  selfdraw_cache_maintain();
  PERF_COUNT(buffer_begin_calls, 1);
  if (!paint_buffer_usable()) {
    paint_buf_stale = true;  // direct painting bypasses the buffer
    if (!cfg.display_buffering) {
      // free the buffer when disabled (but keep it across the dynamic
      // bypasses, which are usually transient)
      paint_buffer_drop();
    }
    return false;
  }
  RECT cr;
  GetClientRect(wnd, &cr);
  int w = cr.right - cr.left;
  int h = cr.bottom - cr.top;
  if (w <= 0 || h <= 0) {
    PERF_COUNT(buffer_reject_size, 1);
    paint_buf_stale = true;
    return false;
  }
  if (!paint_buf_dc || w != paint_buf_w || h != paint_buf_h) {
    // (re)create the buffer at the current client size
    PERF_COUNT(buffer_recreate, 1);
    paint_buffer_drop();
    paint_buf_dc = CreateCompatibleDC(dc);
    if (paint_buf_dc) {
      /* Prefer a DIB section over a compatible bitmap: it renders and
         blits the same, but exposes the pixel store, allowing solid
         fills to bypass per-call GDI overhead (win_fill_rect below).
         Top-down orientation (negative height) so row y is at offset
         y * width. On failure, fall back to a compatible bitmap with
         the direct fill path disabled. */
      BITMAPINFO bmi;
      memset(&bmi, 0, sizeof bmi);
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = w;
      bmi.bmiHeader.biHeight = -h;
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      void * bits = 0;
      paint_buf_bm = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, 0, 0);
      paint_buf_bits = paint_buf_bm ? (uint *)bits : 0;
      if (!paint_buf_bm) {
        paint_buf_bm = CreateCompatibleBitmap(dc, w, h);
      }
    }
    if (!paint_buf_bm) {
      PERF_COUNT(buffer_reject_create, 1);
      paint_buffer_drop();
      return false;
    }
    SelectObject(paint_buf_dc, paint_buf_bm);
    paint_buf_w = w;
    paint_buf_h = h;
    paint_buf_stale = true;
  }
  if (paint_buf_stale) {
    /* Initialise the terminal viewport, including its padding, before the
       forced repaint. Do not copy from the window DC here: do_update may
       have a horizontal-scroll world transform active on that DC. Graphic
       backgrounds repaint this seed; a failed or absent background leaves
       the same solid padding colour used by win_paint. */
    int paint_bottom = h - (win_search_visible() ? SEARCHBAR_HEIGHT : 0);
    RECT viewport = {0, OFFSET, w, paint_bottom};
    colour bg = colours[term.rvideo ? FG_COLOUR_I : BG_COLOUR_I];
    SetDCBrushColor(paint_buf_dc, bg);
    FillRect(paint_buf_dc, &viewport, GetStockObject(DC_BRUSH));
    PERF_COUNT(buffer_stale_full_repaint, 1);
    term_invalidate(0, 0, term.cols - 1, term_allrows - 1);
    paint_buf_stale = false;
  }
  assert(!paint_buffered && !paint_win_dc);
  paint_win_dc = dc;
  dc = paint_buf_dc;
  paint_buffered = true;
  // bound any push/pop imbalance to a single frame
  paint_dc_busy = 0;
  /* One flush per frame: operations on DIB-section DCs execute
     unbatched (empirically: SelectObject/ExtTextOut on this DC cost a
     kernel transition each), so per-fill flushing is redundant; this
     single flush covers any operations still pending from elsewhere. */
  GdiFlush();
  // reset the dirty row span; win_text extends it as it paints
  paint_dirty_top = term_allrows;
  paint_dirty_bot = -1;
  PERF_COUNT(buffer_used, 1);
  return true;
}

/*
 * Finish a buffered paint: point the global dc back at the window
 * target and transfer the painted rows from the buffer to the window
 * in one blit. The row span is padded by one row on each side to cover
 * double-height glyphs and vertical overhang. Transfer the full client
 * width because edge runs can paint into the horizontal padding. Include
 * the top or bottom client padding when the corresponding edge rows are
 * covered; image backgrounds deliberately extend into those areas.
 * Clip regions already set on the window DC (search bar exclusion,
 * WM_PAINT update region) restrict the blit just as they restricted
 * direct painting before.
 */
static void
win_paint_buffer_end(void)
{
  assert(paint_buffered && paint_win_dc);
  dc = paint_win_dc;
  paint_win_dc = 0;
  paint_buffered = false;
  if (paint_dirty_bot < 0) {
    return;  // nothing painted, nothing to transfer
  }
  assert(paint_dirty_top <= paint_dirty_bot);
  int top = max(0, paint_dirty_top - 1);
  int bot = min(term_allrows, paint_dirty_bot + 2);  // exclusive
  int x = 0;
  int y = top ? OFFSET + PADDING + top * cell_height : OFFSET;
  int paint_bottom = paint_buf_h -
                     (win_search_visible() ? SEARCHBAR_HEIGHT : 0);
  int bottom = OFFSET + PADDING + bot * cell_height;
  if (paint_dirty_bot >= term.rows - 1 || bot == term_allrows) {
    bottom = paint_bottom;
  }
  bottom = min(bottom, paint_bottom);
  int w = paint_buf_w;
  int h = bottom - y;
  if (w <= 0 || h <= 0) {
    return;
  }
  PERF_SET(paint_dirty_top, paint_dirty_top);
  PERF_SET(paint_dirty_bot, paint_dirty_bot);
  PERF_COUNT(bitblt_calls, 1);
  PERF_COUNT(bitblt_pixels, (uint64_t)w * (uint64_t)h);
  PERF_COUNT(bitblt_width, w);
  PERF_COUNT(bitblt_height, h);
  long long perf_t0 = mintty_perf_ticks();
  BitBlt(dc, x, y, w, h, paint_buf_dc, x, y, SRCCOPY);
  PERF_ADD_TICKS(bitblt_ticks, mintty_perf_ticks() - perf_t0);
}

/*
 * Fill rectangle *r on the global dc with solid colour c.
 * When painting is routed into the DIB back buffer and the DC state is
 * trivial - identity world transform and no clip region, verified by
 * querying the DC so that correctness never depends on tracking every
 * transform/clip site - the fill is performed by writing the pixel
 * store directly. This bypasses the per-call GDI overhead of FillRect,
 * measured at ~12us per call regardless of size, which dominated paint
 * time at millions of small background and box-glyph fills per session.
 * In all other cases (unbuffered painting, plain-bitmap fallback,
 * active transform or clip), an ordinary FillRect with the
 * recolourable stock brush is issued; the resulting pixels are
 * identical either way.
 */
static void
win_fill_rect(const RECT * r, colour c)
{
  if (paint_buffered && paint_buf_bits && paint_dc_busy == 0) {
#ifndef NDEBUG
    // cross-check the tracked state against the DC: an unbalanced
    // transform/clip pair anywhere in the paint path trips this
    bool dbg_plain = true;
    if (GetGraphicsMode(dc) == GM_ADVANCED) {
      XFORM xf;
      dbg_plain = GetWorldTransform(dc, &xf)
              && xf.eM11 == 1.0f && xf.eM12 == 0.0f
              && xf.eM21 == 0.0f && xf.eM22 == 1.0f
              && xf.eDx  == 0.0f && xf.eDy  == 0.0f;
    }
    if (dbg_plain) {
      if (!paint_buf_scratch_rgn) {
        paint_buf_scratch_rgn = CreateRectRgn(0, 0, 0, 0);
      }
      dbg_plain = paint_buf_scratch_rgn
              && GetClipRgn(dc, paint_buf_scratch_rgn) == 0;
    }
    assert(dbg_plain);
#endif
    {
      int left   = max(0, (int)r->left);
      int top    = max(0, (int)r->top);
      int right  = min(paint_buf_w, (int)r->right);
      int bottom = min(paint_buf_h, (int)r->bottom);
      if (left >= right || top >= bottom) {
        return;
      }
      // BI_RGB 32bpp stores 0x00RRGGBB words; colour is COLORREF
      // 0x00BBGGRR, so swap the red and blue channels
      uint pix = ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
      for (int fy = top; fy < bottom; fy++) {
        uint * row = paint_buf_bits + (size_t)fy * paint_buf_w + left;
        for (int fx = 0; fx < right - left; fx++) {
          row[fx] = pix;
        }
      }
      return;
    }
  }
  SetDCBrushColor(dc, c);
  FillRect(dc, r, GetStockObject(DC_BRUSH));
}

/*
 * DC for painting terminal overlay graphics (emoji images) on:
 * the back buffer while a buffered display update is in progress
 * (so that the subsequent blit includes them), else a fresh window DC
 * as before. Pair each call with win_release_paint_dc.
 */
HDC
win_get_paint_dc(void)
{
  if (paint_buffered) {
    assert(dc == paint_buf_dc);
    return dc;
  }
  return GetDC(wnd);
}

/*
 * Release a DC obtained from win_get_paint_dc; pdc is the value that
 * call returned. Releases the window DC in the unbuffered case and is
 * a no-op for the shared back buffer DC.
 */
void
win_release_paint_dc(HDC pdc)
{
  if (!paint_buffered) {
    ReleaseDC(wnd, pdc);
  }
  else {
    assert(pdc == paint_buf_dc);
  }
}

#define update_timer 16

void
do_update(void)
{
  //if (kb_trace) printf("[%ld] do_update\n", mtime());

#if defined(debug_cursor) && debug_cursor > 1
  printf("do_update cursor_on %d @%d,%d\n", term.cursor_on, term.curs.y, term.curs.x);
#endif
  //printf("do_update state %d susp %d\n", update_state, term.suspend_update);
  if (update_state == UPDATE_BLOCKED) {
    update_state = UPDATE_IDLE;
    return;
  }

  update_skipped++;
  int output_lines_scrolled = lines_scrolled;
  int output_speed = output_lines_scrolled / (term.rows ?: cfg.rows);
  lines_scrolled = 0;
  bool iconic = !term.detect_progress && win_is_iconic();
  if ((update_skipped < cfg.display_speedup && cfg.display_speedup < 10
       && output_speed > update_skipped
       //&& !term.smooth_scroll ?
      ) || iconic
        //|| win_is_hidden() ?
        // suspend display update:
        //|| (update_skipped < term.suspend_update * cfg.display_speedup)
        || (update_skipped * update_timer < term.suspend_update)
     )
  {
    //printf("skip %d susp %d\n", update_skipped, term.suspend_update);
    mintty_perf_note_update_skip(update_skipped, cfg.display_speedup,
                                 output_speed, output_lines_scrolled,
                                 iconic, term.suspend_update);
    win_set_timer(do_update, update_timer);
    return;
  }
  int logged_update_skipped = update_skipped;
  update_skipped = 0;
  term.suspend_update = 0;

  update_state = UPDATE_BLOCKED;

  mintty_perf_update_begin("update");
  PERF_SET(cols, term.cols);
  PERF_SET(rows, term.rows);
  PERF_SET(allrows, term_allrows);
  PERF_SET(cell_width, cell_width);
  PERF_SET(cell_height, cell_height);
  PERF_SET(display_speedup, cfg.display_speedup);
  PERF_SET(display_buffering, cfg.display_buffering);
  PERF_SET(ligatures, cfg.ligatures);
  PERF_SET(font_render, cfg.font_render);
  PERF_SET(output_lines_scrolled, output_lines_scrolled);
  PERF_SET(output_speed, output_speed);
  PERF_SET(update_skipped, logged_update_skipped);
  PERF_SET(disptop, term.disptop);

  show_curchar_info('u');

  long long perf_t0 = mintty_perf_ticks();
  dc = GetDC(wnd);
  PERF_ADD_TICKS(getdc_ticks, mintty_perf_ticks() - perf_t0);

  // horizontal scrolling of terminal view
  int dx = - horclip();
  if (dx) {
    XFORM xform = (XFORM){1.0, 0.0, 0.0, 1.0, (float)dx, 0.0};
    if (SetGraphicsMode(dc, GM_ADVANCED))
      SetWorldTransform(dc, &xform);
  }

  perf_t0 = mintty_perf_ticks();
  win_paint_exclude_search(dc);
  term_update_search();
  PERF_ADD_TICKS(search_ticks, mintty_perf_ticks() - perf_t0);

  if (tek_mode)
    tek_paint();
  else {
    perf_t0 = mintty_perf_ticks();
    bool buffered = win_paint_buffer_begin();
    PERF_ADD_TICKS(paint_buffer_begin_ticks, mintty_perf_ticks() - perf_t0);

    perf_t0 = mintty_perf_ticks();
    term_paint();
    PERF_ADD_TICKS(term_paint_ticks, mintty_perf_ticks() - perf_t0);

    if (buffered) {
      perf_t0 = mintty_perf_ticks();
      win_paint_buffer_end();
      PERF_ADD_TICKS(paint_buffer_end_ticks, mintty_perf_ticks() - perf_t0);
    }

    perf_t0 = mintty_perf_ticks();
    winimgs_paint();
    PERF_ADD_TICKS(winimgs_paint_ticks, mintty_perf_ticks() - perf_t0);
  }

  perf_t0 = mintty_perf_ticks();
  ReleaseDC(wnd, dc);
  PERF_ADD_TICKS(release_dc_ticks, mintty_perf_ticks() - perf_t0);

  // Update scrollbar
  perf_t0 = mintty_perf_ticks();
 /* Skip the call when nothing changed: SetScrollInfo redraws the
    scrollbar synchronously even when passed identical values, which
    measures at more than a millisecond per display update. The cached
    values are only trusted while this branch remains the sole writer
    of SB_VERT: whenever the branch is not taken (scrollbar hidden or
    taken over by an application scrollbar via DECSET 30 handling), the
    cache is invalidated so the next update writes unconditionally. */
  static bool scrollinfo_cached = false;
  static int scrollinfo_max, scrollinfo_page, scrollinfo_pos;
  if (cfg.scrollbar && term.show_scrollbar && !term.app_scrollbar) {
    int lines = sblines();
    SCROLLINFO si = {
      .cbSize = sizeof si,
      .fMask = SIF_ALL | SIF_DISABLENOSCROLL,
      .nMin = 0,
      .nMax = lines + term.rows - 1,
      .nPage = term.rows,
      .nPos = lines + term.disptop
    };
    if (!scrollinfo_cached
        || si.nMax != scrollinfo_max
        || (int)si.nPage != scrollinfo_page
        || si.nPos != scrollinfo_pos
       )
    {
      SetScrollInfo(wnd, SB_VERT, &si, true);
      scrollinfo_cached = true;
      scrollinfo_max  = si.nMax;
      scrollinfo_page = (int)si.nPage;
      scrollinfo_pos  = si.nPos;
    }
  }
  else {
    scrollinfo_cached = false;
  }
  PERF_ADD_TICKS(scrollbar_ticks, mintty_perf_ticks() - perf_t0);

  // Update the positions of the system caret and the IME window.
  // (We maintain a caret, even though it's invisible, for the benefit of
  // blind people: apparently some helper software tracks the system caret,
  // so we should arrange to have one.)
  perf_t0 = mintty_perf_ticks();
  if (term.has_focus) {
    int x = term.curs.x * cell_width + PADDING;
    int y = (term.curs.y - term.disptop) * cell_height + OFFSET + PADDING;
    SetCaretPos(x, y);
    if (ime_open) {
      COMPOSITIONFORM cf = {.dwStyle = CFS_POINT, .ptCurrentPos = {x, y}};
      ImmSetCompositionWindow(imc, &cf);
    }
  }
  PERF_ADD_TICKS(caret_ticks, mintty_perf_ticks() - perf_t0);

  // Schedule next update.
  win_set_timer(do_update, update_timer);
  mintty_perf_update_end();
}

#include <math.h>

/*
   Indicate size of selection with a popup tip (option SelectionShowSize).
   Future enhancements may be automatic position flipping depending 
   on selection direction or if the tip reaches outside the screen.
   Also the actual tip window should better be decoupled from the 
   window size tip which is now abused for this feature.
 */
static void
sel_update(bool update_sel_tip)
{
  static bool selection_tip_active = false;
  //printf("sel_update tok %d sel %d act %d\n", tip_token, term.selected, selection_tip_active);
  if (term.selected && cfg.selection_show_size && update_sel_tip) {
    int cols, rows;
    if (term.sel_rect) {
      rows = abs(term.sel_end.y - term.sel_start.y) + 1;
      cols = abs(term.sel_end.x - term.sel_start.x);
    }
    else {
      rows = term.sel_end.y - term.sel_start.y + 1;
      if (rows == 1)
        cols = term.sel_end.x - term.sel_start.x;
      else
        cols = term.cols;
    }
    RECT wr;
    GetWindowRect(wnd, &wr);
    LONG style = GetWindowLong(wnd, GWL_STYLE);
    int x = wr.left
          + ((style & WS_THICKFRAME) ? GetSystemMetrics(SM_CXSIZEFRAME) : 0)
          + PADDING + last_pos.x * cell_width;
    int y = wr.top
          + ((style & WS_THICKFRAME) ? GetSystemMetrics(SM_CYSIZEFRAME) : 0)
          + ((style & WS_CAPTION) ? GetSystemMetrics(SM_CYCAPTION) : 0)
          + OFFSET + PADDING + last_pos.y * cell_height;
#ifdef debug_selection_show_size 
    cfg.selection_show_size = cfg.selection_show_size % 12 + 1;
#endif
    int w = 30, h = 18;  // assumed size of tip window
    float phi = 2 * 3.1415 / 12 * (cfg.selection_show_size + 9);
    float rx = cell_width * 1.5;
    float ry = cell_height * 1.5;
    int dx = cell_width / 2 + rx * cos(phi) - w / 2;
    int dy = cell_height / 2 + ry * sin(phi) - h / 2;
    //printf("selection_show_size [%d]: %.2f %.2f %.2f\n", cfg.selection_show_size, phi, cos(phi), sin(phi));
    win_show_tip(x + dx, y + dy, cols, rows);
    selection_tip_active = true;
  }
  else if (!term.selected && selection_tip_active) {
    win_destroy_tip();
    selection_tip_active = false;
  }
}

static void
show_link(void)
{
  static int lasthoverlink = -1;

  int hoverlink = term.hovering ? term.hoverlink : -1;
  if (hoverlink != lasthoverlink) {
    lasthoverlink = hoverlink;

    char * url = geturl(hoverlink) ?: "";

    if (nonascii(url)) {
      wchar * wcs = cs__utftowcs(url);
      SetWindowTextW(wnd, wcs);
      free(wcs);
    }
    else
      SetWindowTextA(wnd, url);
  }
}

void
win_update_now(void)
{
  if (update_state == UPDATE_PENDING)
    update_state = UPDATE_IDLE;
  win_update(false);
}

void
win_update(bool update_sel_tip)
{
  //if (kb_trace) printf("[%ld] win_update state %d (idl/blk/pnd)\n", mtime(), update_state);
  trace_resize(("----- win_update\n"));

  if (update_state == UPDATE_IDLE)
    do_update();
  else
    update_state = UPDATE_PENDING;

  sel_update(update_sel_tip);
  if (cfg.hover_title)
    show_link();
}

void
win_schedule_update(void)
{
  //if (kb_trace) printf("[%ld] win_schedule_update state %d (idl/blk/pnd)\n", mtime(), update_state);

  if (update_state == UPDATE_IDLE)
    win_set_timer(do_update, update_timer);
  update_state = UPDATE_PENDING;
}


static void
another_font(struct fontfam * ff, int fontno)
{
  int basefont;
  int u, w, i, s, x;

  if (fontno < 0 || fontno >= FONT_MAXNO || ff->fontflag[fontno])
    return;

  basefont = (fontno & ~(FONT_BOLDUND));
  if (basefont != fontno && !ff->fontflag[basefont])
    another_font(ff, basefont);

  w = ff->fw_norm;
  i = false;
  s = false;
  u = false;
  x = cell_width;

  if (fontno & FONT_WIDE)
    x *= 2;
  if (fontno & FONT_NARROW)
    x = (x + 1) / 2;
  if (fontno & FONT_BOLD)
    w = ff->fw_bold;
  if (fontno & FONT_ITALIC)
    i = true;
  if (fontno & FONT_STRIKEOUT)
    s = true;
  if (fontno & FONT_UNDERLINE)
    u = true;
  int y = font_height * (1 + !!(fontno & FONT_HIGH));
  if (fontno & FONT_ZOOMFULL) {
    y = cell_height * (1 + !!(fontno & FONT_HIGH));
    x = cell_width * (1 + !!(fontno & FONT_WIDE));
  }
  if (fontno & FONT_ZOOMSMALL) {
    y = y * 12 / 20;
    x = x * 12 / 20;
  }
  if (fontno & FONT_ZOOMDOWN) {
    y = y / 2;
    x = x / 2;
  }

#ifdef debug_create_font
  printf("another_font: font [%02X]: %d (size %d%s%s%s%s) %d w%4d i%d u%d s%d\n", 
	fontno, font_height * (1 + !!(fontno & FONT_HIGH)), font_size, 
	fontno & FONT_HIGH     ? " hi" : "",
	fontno & FONT_WIDE     ? " wd" : "",
	fontno & FONT_NARROW   ? " nr" : "",
	fontno & FONT_ZOOMFULL ? " zf" : "",
	x, w, i, u, s);
#endif
  if (fontno & FONT_DIM) {
    wchar name[wcslen(ff->name) + 7];
    wcscpy(name, ff->name);
    wcscat(name, W(" Light"));
    ff->fonts[fontno] =
      CreateFontW(y, x, 0, 0, w, i, u, s,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  get_font_quality(), FIXED_PITCH | FF_DONTCARE, name);
  }
  else
    ff->fonts[fontno] =
      CreateFontW(y, x, 0, 0, w, i, u, s,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  get_font_quality(), FIXED_PITCH | FF_DONTCARE, ff->name);

  ff->fontflag[fontno] = true;
}

void
win_set_ime_open(bool open)
{
  if (open != ime_open) {
    ime_open = open;
    term.cursor_invalid = true;
    win_update(false);
  }
}


/*
   Background texture/image.
 */

static bool tiled = false;
static bool ratio = false;
static bool wallp = false;
static bool multi = false;
static int wallp_style;
static int alpha = -1;
static LONG w = 0, h = 0;
static HBRUSH bgbrush_bmp = 0;

static BOOL (WINAPI *pAlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION) = 0;

static HBITMAP
alpha_blend_bg(int alpha, HDC dc, HBITMAP hbm, int bw, int bh, colour bg)
{
  // load GDI function
  if (!pAlphaBlend) {
    pAlphaBlend = load_library_func("msimg32.dll", "AlphaBlend");
  }
  if (!pAlphaBlend)
    return hbm;

  // take size from hbm if not passed explicitly
  if (!bw || !bh) {
    BITMAP bm0;
    if (!GetObject(hbm, sizeof(BITMAP), &bm0))
      return hbm;
    bw = bm0.bmWidth;
    bh = bm0.bmHeight;
  }

  // prepare source memory DC and select the source bitmap into it
  HDC dc0 = CreateCompatibleDC(dc);
  HBITMAP oldhbm0 = SelectObject(dc0, hbm);

  // prepare destination memory DC, 
  // create and select the destination bitmap into it
  HDC dc1 = CreateCompatibleDC(dc);
  HBITMAP hbm1 = CreateCompatibleBitmap(dc0, bw, bh);
  HBITMAP oldhbm1 = SelectObject(dc1, hbm1);

  HBRUSH bgb = CreateSolidBrush(bg);
  FillRect(dc1, &(RECT){0, 0, bw, bh}, bgb);
  DeleteObject(bgb);

  BYTE alphafmt = alpha == 255 ? AC_SRC_ALPHA : 0;
  BLENDFUNCTION bf = (BLENDFUNCTION) {AC_SRC_OVER, 0, alpha, alphafmt};
  int ok = pAlphaBlend(dc1, 0, 0, bw, bh, dc0, 0, 0, bw, bh, bf);

  // release everything
  SelectObject(dc1, oldhbm1);
  SelectObject(dc0, oldhbm0);
  DeleteDC(dc1);
  DeleteDC(dc0);

  if (ok) {
    DeleteObject(hbm);
    return hbm1;
  }
  else
    return hbm;
}

static void
offset_bg(HDC dc)
{
  RECT wr;
  GetWindowRect(wnd, &wr);
  int wx = wr.left + GetSystemMetrics(SM_CXSIZEFRAME);
  int wy = wr.top + GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CYCAPTION);

  // adjust brush to virtual desktop (#1296)
static int virtual_desktop_left;
static int virtual_desktop_top;
  if (!checked_desktop_config) {
    HWND dt = GetDesktopWindow();
    HDC dtc = GetDC(dt);
    GetClipBox(dtc, &wr);
    ReleaseDC(dt, dtc);
    virtual_desktop_left = wr.left;
    virtual_desktop_top = wr.top;

    LONG exstyle = GetWindowLong(wnd, GWL_EXSTYLE);
    if (exstyle & WS_EX_LEFTSCROLLBAR)
      virtual_desktop_left -= GetSystemMetrics(SM_CXVSCROLL);
    LONG style = GetWindowLong(wnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME)) {  // BorderStyle=void
      virtual_desktop_top += GetSystemMetrics(SM_CYSIZEFRAME);// + GetSystemMetrics(SM_CYCAPTION);
      virtual_desktop_left += GetSystemMetrics(SM_CXSIZEFRAME);
    }
    if (!(style & WS_CAPTION))  // BorderStyle=frame
      virtual_desktop_top += GetSystemMetrics(SM_CYCAPTION);

    checked_desktop_config = true;
  }
  wx -= virtual_desktop_left;
  wy -= virtual_desktop_top;

  // adjust wallpaper origin

  SetBrushOrgEx(dc, -wx, -wy, 0);
}

#if CYGWIN_VERSION_API_MINOR >= 74

#include <w32api/wtypes.h>
#include <w32api/gdiplus/gdiplus.h>
#include <w32api/gdiplus/gdiplusflat.h>

static GpBrush * bgbrush_img = 0;
static GpGraphics * bg_graphics = 0;

#define dont_debug_gdiplus

#ifdef debug_gdiplus
static void
gpcheck(char * tag, GpStatus s)
{
  static char * gps[] = {
    "Ok",
    "GenericError",
    "InvalidParameter",
    "OutOfMemory",
    "ObjectBusy",
    "InsufficientBuffer",
    "NotImplemented",
    "Win32Error",
    "WrongState",
    "Aborted",
    "FileNotFound",
    "ValueOverflow",
    "AccessDenied",
    "UnknownImageFormat",
    "FontFamilyNotFound",
    "FontStyleNotFound",
    "NotTrueTypeFont",
    "UnsupportedGdiplusVersion",
    "GdiplusNotInitialized",
    "PropertyNotFound",
    "PropertyNotSupported",
    "ProfileNotFound",
  };
#if debug_gdiplus <= 1
  if (s)
#endif
    printf("[%s] %d %s\n", tag, s, s >= 0 && s < lengthof(gps) ? gps[s] : "?");
}
#else
#define gpcheck(tag, s)	(void)s
#endif

static void
drop_background_image_brush(void)
{
  if (bgbrush_img) {
    GpStatus s = GdipDeleteBrush(bgbrush_img);
    gpcheck("delete brush", s);
    bgbrush_img = 0;
  }
}

static void
init_gdiplus(void)
{
  static GdiplusStartupInput gi = {1, NULL, FALSE, FALSE};
  static ULONG_PTR gis = 0;
  if (!gis) {
    GpStatus s = GdiplusStartup(&gis, &gi, NULL);
    gpcheck("startup", s);
  }
}

static bool
get_image_size(wstring fn, uint * _bw, uint * _bh)
{
  GpStatus s;

  init_gdiplus();

  GpBitmap * gbm = 0;
  s = GdipCreateBitmapFromFile(fn, &gbm);
  gpcheck("bitmap from file", s);
  if (s != Ok || !gbm)
    return false;

  GpStatus stsz = GdipGetImageWidth(gbm, _bw);
  gpcheck("get size", stsz);
  if (stsz == Ok) {
    stsz = GdipGetImageHeight(gbm, _bh);
    gpcheck("get size", stsz);
  }

  s = GdipDisposeImage(gbm);
  gpcheck("dispose bitmap", s);

  if (stsz != Ok) {
    HBITMAP hbm = 0;
    s = GdipCreateHBITMAPFromBitmap(gbm, &hbm, 0);
    gpcheck("convert bitmap", s);

    BITMAP bm0;
    if (!GetObject(hbm, sizeof(BITMAP), &bm0))
      return false;
    *_bw = bm0.bmWidth;
    *_bh = bm0.bmHeight;
    DeleteObject(hbm);
  }
  return true;
}

static void
load_background_image_brush(HDC dc, wstring fn)
{
  GpStatus s;

  init_gdiplus();

  drop_background_image_brush();

  // try to provide a GDI brush from a GDI+ image
  // (because a GDI brush is much more efficient than a GDI+ brush)
  GpBitmap * gbm = 0;
  s = GdipCreateBitmapFromFile(fn, &gbm);
  gpcheck("bitmap from file", s);

  if (s == Ok && gbm) {
    HBITMAP hbm = 0;
    s = GdipCreateHBITMAPFromBitmap(gbm, &hbm, 0);
    gpcheck("convert bitmap", s);

    if (!tiled) {
      // scale the bitmap; 
      // wtf is this a complex task @ braindamaged Windows API
      // https://www.experts-exchange.com/questions/28594399/Whats-the-best-way-to-scale-a-windows-bitmap.html

      uint bw, bh;
      GpStatus stsz = GdipGetImageWidth(gbm, &bw);
      gpcheck("get size", stsz);
      if (stsz == Ok) {
        stsz = GdipGetImageHeight(gbm, &bh);
        gpcheck("get size", stsz);
      }

      s = GdipDisposeImage(gbm);
      gpcheck("dispose bitmap", s);
      gbm = 0;

      if (stsz != Ok) {
        BITMAP bm0;
        if (!GetObject(hbm, sizeof(BITMAP), &bm0))
          return;
        bw = bm0.bmWidth;
        bh = bm0.bmHeight;
      }

#ifdef scale_to_aspect_ratio_asynchronously
      // keep aspect ratio of background image if requested
      static wchar * prevbg = 0;
      bool isnewbg = !prevbg || wcscmp(cfg.background, prevbg);
      printf("isnewbg %d <%ls> <%ls>\n", isnewbg, cfg.background, prevbg);
      if (ratio && isnewbg && abs((int)bw * h - (int)bh * w) > 5) {
        if (prevbg)
          free(prevbg);
        prevbg = wcsdup(cfg.background);

        int xh, xw;
        if (bw * h < bh * w) {
          xh = h;
          xw = bw * xh / bh;
        } else {
          xw = w;
          xh = bh * xw / bw;
        }
        int sy = win_search_visible() ? SEARCHBAR_HEIGHT : 0;
        printf("%dx%d (%dx%d) -> %dx%d\n", (int)h, (int)w, bh, bw, xh, xw);
        // rescale window to aspect ratio of background image
        win_set_pixels(xh - 2 * PADDING - OFFSET - sy, xw - 2 * PADDING);
        // WARNING: rescaling asynchronously at this point makes 
        // terminal geometry (term.rows, term.cols) inconsistent with 
        // running operations and may crash mintty; 
        // postponing the resizing with SendMessage does not help;
        // therefore try to update mintty data now; 
        // this seems to help a bit, but not completely;
        // that's why this embedded approach is disabled
        do_update();
        w = xw;
        h = xh;
      }
#endif

      // prepare source memory DC and select the source bitmap into it
      HDC dc0 = CreateCompatibleDC(dc);
      HBITMAP oldhbm0 = SelectObject(dc0, hbm);
      // crop image for combined scaling and tiling (#1180)
      if (multi) {
        int imgw = w;
        int imgh = h;
        if (bw * h > w * bh) {
          imgw = w;
          imgh = bh * imgw / bw;
        }
        else if (bw * h < w * bh) {
          imgh = h;
          imgw = bw * imgh / bh;
        }
        w = imgw;
        h = imgh;
      }

      // prepare destination memory DC, 
      // create and select the destination bitmap into it
      HDC dc1 = CreateCompatibleDC(dc);
      HBITMAP hbm1 = CreateCompatibleBitmap(dc0, w, h);
      HBITMAP oldhbm1 = SelectObject(dc1, hbm1);

      if (alpha >= 0 && !pAlphaBlend) {
        pAlphaBlend = load_library_func("msimg32.dll", "AlphaBlend");
      }

      if (alpha < 0 || !pAlphaBlend) {
        // set half-tone stretch-blit mode for scaling quality
        SetStretchBltMode(dc1, HALFTONE);
        // draw the bitmap scaled into the destination memory DC
        StretchBlt(dc1, 0, OFFSET, w, h - OFFSET, dc0, 0, 0, bw, bh, SRCCOPY);

        DeleteObject(hbm);
        hbm = hbm1;
      }
      else {
#ifdef fill_bg_with_rectangle
#warning missing border HPEN
        HBRUSH oldbrush = SelectObject(dc1, CreateSolidBrush(win_get_colour(BG_COLOUR_I)));
        Rectangle(dc1, 0, 0, w, h);
        DeleteObject(SelectObject(dc1, oldbrush));
#else
        HBRUSH br = CreateSolidBrush(win_get_colour(BG_COLOUR_I));
        FillRect(dc1, &(RECT){0, 0, w, h}, br);
        DeleteObject(br);
#endif

        BYTE alphafmt = alpha == 255 ? AC_SRC_ALPHA : 0;
        BLENDFUNCTION bf = (BLENDFUNCTION) {AC_SRC_OVER, 0, alpha, alphafmt};
        if (pAlphaBlend(dc1, 0, OFFSET, w, h - OFFSET, dc0, 0, 0, bw, bh, bf)) {
          DeleteObject(hbm);
          hbm = hbm1;
        }
      }

      // release everything
      SelectObject(dc1, oldhbm1);
      SelectObject(dc0, oldhbm0);
      DeleteDC(dc1);
      DeleteDC(dc0);
    }
    else {  // tiled
      if (alpha >= 0) {
        uint bw = 0, bh = 0;
        s = GdipGetImageWidth(gbm, &bw);
        gpcheck("get size", s);
        s = GdipGetImageHeight(gbm, &bh);
        gpcheck("get size", s);
        hbm = alpha_blend_bg(alpha, dc, hbm, bw, bh, win_get_colour(BG_COLOUR_I));
      }

      s = GdipDisposeImage(gbm);
      gpcheck("dispose bitmap", s);
      gbm = 0;
    }

    // now we have the (scaled or to-be-tiled) bitmap in 'hbm'
    if (hbm) {
      bgbrush_bmp = CreatePatternBrush(hbm);
      DeleteObject(hbm);
      if (bgbrush_bmp) {
        RECT cr;
        GetClientRect(wnd, &cr);
        // support tabbar
        cr.top += OFFSET;

        /* By applying this tweak here and (!) in fill_background below,
           we can apply an offset to the origin of a wallpaper background, 
           in order to simulate a floating window.
         */
        if (wallp) {
          offset_bg(dc);
        }

        FillRect(dc, &cr, bgbrush_bmp);
        drop_background_image_brush();
        return;
      }
    }
  }

#ifdef use_gdiplus_brush_fallback
  DWORD win_version = GetVersion();
  win_version = ((win_version & 0xff) << 8) | ((win_version >> 8) & 0xff);
  if (win_version > 0x0601)  // not Windows 7 or XP
    return;

  // creating a GDI brush failed,
  // try to provide a GDI+ brush (does not work on Windows 10)
  GpImage * img = 0;
  s = GdipLoadImageFromFile(fn, &img);
  gpcheck("load image", s);

  GpTexture * gt = 0;
  s = GdipCreateTexture(img, WrapModeTile, &gt);
  gpcheck("texture", s);
  if (!tiled) {
    uint iw, ih;
    s = GdipGetImageWidth(img, &iw);
    gpcheck("width", s);
    s = GdipGetImageHeight(img, &ih);
    gpcheck("height", s);
    s = GdipScaleTextureTransform(gt, (float)w / iw, (float)h / ih, 0);
    gpcheck("scale", s);
  }
  s = GdipDisposeImage(img);
  gpcheck("dispose img", s);

  bgbrush_img = gt;
#endif
}

static bool
fill_rect(HDC dc, RECT * boxp, GpBrush * br)
{
  GpStatus s, sbrush = -1;
#ifdef debug_gdiplus
  static int nfills = 0;
  nfills ++;
#endif

  void fill(void)
  {
    sbrush = GdipFillRectangleI(bg_graphics, br, boxp->left, boxp->top, boxp->right - boxp->left, boxp->bottom - boxp->top);
    gpcheck("fill", sbrush);
  }

  if (bg_graphics) {
    fill();
  }
  if (sbrush != Ok) {
    if (bg_graphics) {
      s = GdipDeleteGraphics(bg_graphics);
      gpcheck("delete graphics", s);
      bg_graphics = 0;
    }
#ifdef debug_gdiplus
    printf("creating graphics, failure rate 1/%d\n", nfills);
    nfills = 0;
#endif
    s = GdipCreateFromHDC(dc, &bg_graphics);
    gpcheck("create graphics", s);
    fill();
  }

  return sbrush == Ok;
}

#endif

void
win_flush_background(bool clearbg)
{
#if defined(debug_gdiplus) && debug_gdiplus > 2
  printf("flush background bmp %d img %d gr %d (tiled %d)\n", !!bgbrush_bmp, !!bgbrush_img, !!bg_graphics, tiled);
#endif
  w = 0; h = 0;
  tiled = false;
  if (clearbg) {
    alpha = -1;
    // TODO: save redundant image reloading (and brush creation)
  }

  if (bgbrush_bmp) {
    DeleteObject(bgbrush_bmp);
    bgbrush_bmp = 0;
  }
#if CYGWIN_VERSION_API_MINOR >= 74
  drop_background_image_brush();
  GpStatus s;
  if (bg_graphics) {
    s = GdipDeleteGraphics(bg_graphics);
    bg_graphics = 0;
    gpcheck("delete graphics", s);
  }
#endif
}

/*
   Return background image filename in malloced string.
 */
static wchar *
get_bg_filename(void)
{
  tiled = false;
  ratio = false;
  wallp = false;
  multi = false;
  static wchar * wallpfn = 0;

  wchar * bgfn = (wchar *)cfg.background;
  if (*bgfn == '*') {
    tiled = true;
    bgfn++;
  }
  else if (*bgfn == '%') {
    ratio = true;
    bgfn++;
  }
  else if (*bgfn == '+') {
    multi = true;
    bgfn++;
  }
  else if (*bgfn == '_') {
    bgfn++;
  }
#if CYGWIN_VERSION_API_MINOR >= 74
  else if (*bgfn == '=') {
    wallp = true;

    if (!wallpfn)
      wallpfn = newn(wchar, MAX_PATH + 1);

    void readregstr(HKEY key, wstring attribute, wchar * val, DWORD len) {
      DWORD type;
      int err = RegQueryValueExW(key, attribute, 0, &type, (void *)val, &len);
      if (err ||
          !(type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
         )
        *val = 0;
    }

    HKEY wpk = 0;
    RegOpenKeyA(HKEY_CURRENT_USER, "Control Panel\\Desktop", &wpk);
    if (wpk) {
      readregstr(wpk, W("Wallpaper"), wallpfn, MAX_PATH);
      wchar regval[22];
      readregstr(wpk, W("TileWallpaper"), regval, lengthof(regval));
      tiled = 0 == wcscmp(regval, W("1"));
      readregstr(wpk, W("WallpaperStyle"), regval, lengthof(regval));
      wallp_style = wcstol(regval, 0, 0);
      //printf("wallpaper <%ls> tiled %d style %d\n", wallpfn, tiled, wallp_style);

      if (tiled && !wallp_style) {
        // can be used as brush directly
      }
      else {
        // need to scale wallpaper later, when loading;
        // not implemented, invalidate
        *wallpfn = 0;
        // possibly, according to docs, but apparently ignored, 
        // also determine origin according to
        // readregstr(wpk, W("WallpaperOriginX"), ...)
        // readregstr(wpk, W("WallpaperOriginY"), ...)
      }
      RegCloseKey(wpk);
    }
  }
#else
  (void)wallp_style;
#endif

  char * bf = cs__wcstombs(bgfn);
  // try to extract an alpha value from file spec
  char * salpha = strrchr(bf, ',');
  if (salpha) {
    *salpha = 0;
    salpha++;
    if (sscanf(salpha, "%u%c", &alpha, &(char){0}) != 1)
      alpha = -1;
  }

  if (wallp)
    bgfn = wcsdup(wallpfn);
  else {
    // path transformations:
    // for dynamic changes (OSC 11) they are already handled 
    // before setting cfg.background in termout.c,
    // but for static configuration (option Background) they 
    // need to be applied here as well
    if (0 == strncmp("~/", bf, 2)) {
      char * bfexp = asform("%s/%s", home, bf + 2);
      free(bf);
      bf = bfexp;
    }
    else if (*bf != '/' && !(*bf && bf[1] == ':')) {
      char * fgd = foreground_cwd();
      if (fgd) {
        char * bfexp = asform("%s/%s", fgd, bf);
        free(bf);
        bf = bfexp;
      }
    }

#ifdef pathname_conversion_here
#warning now deprecated; handled via guardpath
    if (support_wsl && !wallp) {
      wchar * wbf = cs__utftowcs(bf);
      wchar * wdewbf = dewsl(wbf);  // free(wbf)
      char * dewbf = cs__wcstoutf(wdewbf);
      free(wdewbf);
      free(bf);
      bf = dewbf;
    }
#endif

    bgfn = path_posix_to_win_w(bf);
  }

#ifdef debug_gdiplus
  printf("loading brush <%ls> <%s> <%ls>\n", cfg.background, bf, bgfn);
#endif
  free(bf);
  return bgfn;
}

static void
load_background_brush(HDC dc)
{
  // we could try to hook into win_adapt_term_size to update the full 
  // screen background and reload the background on demand, 
  // but let's rather handle this autonomously here
  RECT cr;
  GetClientRect(wnd, &cr);
  if (cr.right - cr.left == w && cr.bottom - cr.top == h)
    return;  // keep brush

  if (tiled)
    return;  // do not scale tiled brush

  // remember terminal screen size
  w = cr.right - cr.left;
  h = cr.bottom - cr.top;

  // adjust paint screen size
  if (win_search_visible())
    cr.bottom -= SEARCHBAR_HEIGHT;

  // support tabbar (minor need here)
  cr.top += OFFSET;

  wchar * bgfn = get_bg_filename();  // also set tiled and alpha

  HBITMAP
  load_background_bitmap(wstring fn)
  {
    HBITMAP bm = 0;
    wstring bmpsuf = wcscasestr(fn, W(".bmp"));
    if (bmpsuf && wcslen(bmpsuf) == 4) {
      if (tiled)
        bm = (HBITMAP) LoadImageW(0, fn,
                                  IMAGE_BITMAP, 0, 0,
                                  LR_DEFAULTSIZE |
                                  LR_LOADFROMFILE);
      else
        bm = (HBITMAP) LoadImageW(0, fn,
                                  IMAGE_BITMAP, w, h,
                                  LR_LOADFROMFILE);
    }

    if (bm && alpha >= 0) {
      if (tiled)
        bm = alpha_blend_bg(alpha, dc, bm, 0, 0, win_get_colour(BG_COLOUR_I));
      else
        bm = alpha_blend_bg(alpha, dc, bm, w, h, win_get_colour(BG_COLOUR_I));
    }

    return bm;
  }

  if (!bgbrush_bmp) {
    HBITMAP bm = load_background_bitmap(bgfn);
    if (bm) {
      bgbrush_bmp = CreatePatternBrush(bm);
      DeleteObject(bm);
      if (bgbrush_bmp) {
        FillRect(dc, &cr, bgbrush_bmp);
      }
    }
  }

  if (!bgbrush_bmp) {
#if CYGWIN_VERSION_API_MINOR >= 74
    load_background_image_brush(dc, bgfn);
    // can have set bgbrush_img or bgbrush_bmp
    if (bgbrush_img)
      fill_rect(dc, &cr, bgbrush_img);
    // flag failure to load background?
    // this is now detected in win_paint by checking the brushes
    //else if (!bgbrush_bmp)
#endif
    //  // trigger proper win_paint behaviour
    //  wstrset(&cfg.background, W(""));  // not the right approach (zooming)
  }
#ifdef debug_gdiplus
  printf("loaded brush <%ls>: GDI %d GDI+ %d (tiled %d)\n", bgfn, !!bgbrush_bmp, !!bgbrush_img, tiled);
#endif

  free(bgfn);
}

bool
fill_background(HDC dc, RECT * boxp)
{
  PERF_COUNT(fill_background_calls, 1);
  long long perf_fill_background_t0 = mintty_perf_ticks();

  load_background_brush(dc);
  if (wallp) {
    offset_bg(dc);
  }

  // support tabbar
  if (boxp->top < OFFSET)
    boxp->top = OFFSET;

  bool res =
    (bgbrush_bmp && FillRect(dc, boxp, bgbrush_bmp))
#if CYGWIN_VERSION_API_MINOR >= 74
    || (bgbrush_img && fill_rect(dc, boxp, bgbrush_img))
#endif
    ;
  PERF_ADD_TICKS(fill_background_ticks, mintty_perf_ticks() - perf_fill_background_t0);
  return res;
}

#define dont_debug_aspect_ratio

void
scale_to_image_ratio()
{
#if CYGWIN_VERSION_API_MINOR >= 74
  if (*cfg.background != '%')
    return;
  wchar * bgfn = get_bg_filename();
#ifdef debug_aspect_ratio
  printf("scale_to_image_ratio <%ls> ratio %d\n", bgfn, ratio);
#endif
  if (!ratio)
    return;

  uint bw, bh;
  int res = get_image_size(bgfn, &bw, &bh);
  free(bgfn);
  if (!res || !bw || !bh)
    return;

  RECT cr;
  GetClientRect(wnd, &cr);
  // remember terminal screen size
  int w = cr.right - cr.left;
  int h = cr.bottom - cr.top;
#ifdef debug_aspect_ratio
  printf("  cur w %d h %d img bw %d bh %d\n", (int)w, (int)h, bw, bh);
#endif

  if (abs((int)bw * h - (int)bh * w) < w + h)
    return;

  w = max(w, ini_width);
  h = max(h, ini_height);
#ifdef debug_aspect_ratio
  printf("  max w %d h %d\n", (int)w, (int)h);
#endif

  int xh, xw;
  if (bw * h < bh * w) {
    xh = h;
    xw = bw * xh / bh;
  } else {
    xw = w;
    xh = bh * xw / bw;
  }
  int sy = win_search_visible() ? SEARCHBAR_HEIGHT : 0;
#ifdef debug_aspect_ratio
  printf("  %dx%d (%dx%d) -> %dx%d\n", (int)w, (int)h, bw, bh, xw, xh);
#endif
  // rescale window to aspect ratio of background image
  win_set_pixels(xh - 2 * PADDING - OFFSET - sy, xw - 2 * PADDING);
#endif
}


/*
   Text output.
 */

#define dont_debug_win_text

#ifdef debug_win_text
static void
_trace_line(char * tag, cattr attr, ushort lattr, wchar * text, int len)
{
  bool show = false;
  for (int i = 0; i < len; i++)
    if (text[i] != ' ')
      show = true;
  if (show) {
    if (*tag != ' ') {
      wchar t[len + 1]; wcsncpy(t, text, len); t[len] = 0;
      printf("%s %04X %08llX <%ls>\n", tag, lattr, attr.attr, t);
    }
    else {
      printf("%s %04X %08llX", tag, lattr, attr.attr);
      for (int i = 0; i < len; i++) printf(" %04X", text[i]);
      printf("\n");
    }
  }
}
#define trace_line(tag) _trace_line(tag, attr, lattr, text, len)
#else
#define trace_line(tag)
#endif


#ifdef substitute_combining_chars
/* Substitution of (some) combining characters by lookalike characters; 
   this has not been needed anymore for a while already (see #295),
   it was dropped for unpleasant side effects:
   for a combined character like x̀, when displayed in two phases 
   (e.g. with background or with cursor), the accent would 
   skip to the next position
*/
static wchar
combsubst(wchar comb, cattrflags attr)
{
  static const struct {
    wchar comb;
    wchar subst;
    short pref;  // -1: suppress, +1: enforce
  } lookup[] = {
    {0x0300, 0x0060, 0},
    {0x0301, 0x00B4, 0},
    {0x0302, 0x02C6, 0},
    {0x0303, 0x02DC, 0},
    {0x0304, 0x00AF, 0},
    {0x0305, 0x203E, 0},
    {0x0306, 0x02D8, 0},
    {0x0307, 0x02D9, 0},
    {0x0308, 0x00A8, 0},
    {0x030A, 0x02DA, 0},
    {0x030B, 0x02DD, 0},
    {0x030C, 0x02C7, 0},
    {0x0327, 0x00B8, 0},
    {0x0328, 0x02DB, 0},
    {0x0332, 0x005F, 0},
    {0x0333, 0x2017, 0},
    {0x033E, 0x2E2F, -1},	// display broken if substituted
    {0x0342, 0x1FC0, +1},	// display broken if not substituted
    {0x0343, 0x1FBD, 0},
    {0x0344, 0x0385, +1},	// display broken if not substituted
    {0x0345, 0x037A, +1},	// display broken if not substituted
    {0x3099, 0x309B, 0},
    {0x309A, 0x309C, 0},
    {0xA67C, 0xA67E, 0},
    {0xA67D, 0xA67F, 0},
  };

  int i, j, k;

  i = -1;
  j = lengthof(lookup);

  while (j - i > 1) {
    k = (i + j) / 2;
    if (comb < lookup[k].comb)
      j = k;
    else if (comb > lookup[k].comb)
      i = k;
    else {
      // apply heuristic tweaking of the substitution:
      if (lookup[k].pref == 1)
        return lookup[k].subst;
      else if (lookup[k].pref == -1)
        return comb;

      wchar chk = comb;
      win_check_glyphs(&chk, 1, attr);
      if (chk)
        return lookup[k].subst;
      else
        return comb;
    }
  }
  return comb;
}
#endif


int
termattrs_equal_fg(cattr * a, cattr * b)
{
  if (a->truefg != b->truefg)
    return false;
#define ATTR_COLOUR_MASK (ATTR_FGMASK | ATTR_BOLD | ATTR_DIM)
  if ((a->attr & ATTR_COLOUR_MASK) != (b->attr & ATTR_COLOUR_MASK))
    return false;
  return true;
}


static int
char1ulen(wchar * text)
{
  if ((text[0] & 0xFC00) == 0xD800 && (text[1] & 0xFC00) == 0xDC00)
    return 2;
  else
    return 1;
}

static SCRIPT_STRING_ANALYSIS ssa;
static bool use_uniscribe;

static void
text_out_start(HDC hdc, LPCWSTR psz, int cch, int *dxs)
{
  long long perf_text_out_start_t0 = mintty_perf_ticks();
  PERF_COUNT(text_out_start_calls, 1);
  PERF_COUNT(text_out_start_chars, cch);
  if (cch == 0) {
    use_uniscribe = false;
    PERF_ADD_TICKS(text_out_start_ticks, mintty_perf_ticks() - perf_text_out_start_t0);
    return;
  }
  if (!use_uniscribe) {
    PERF_ADD_TICKS(text_out_start_ticks, mintty_perf_ticks() - perf_text_out_start_t0);
    return;
  }

#if CYGWIN_VERSION_API_MINOR >= 74
  static SCRIPT_CONTROL sctrl_lig = {.fMergeNeutralItems = 1};
#else
  SCRIPT_CONTROL sctrl_lig = (SCRIPT_CONTROL){.fReserved = 1};
#endif
  PERF_COUNT(uniscribe_analyse_calls, 1);
  PERF_COUNT(uniscribe_analyse_chars, cch);
  long long perf_t0 = mintty_perf_ticks();
  HRESULT hr = ScriptStringAnalyse(hdc, psz, cch, 0, -1, 
    // could | SSA_FIT and use `width` (from win_text) instead of MAXLONG
    // to justify to monospace cell widths;
    // SSA_LINK is needed for Hangul and default-size CJK
    SSA_GLYPHS | SSA_FALLBACK | SSA_LINK, MAXLONG, 
    cfg.ligatures > 1 ? &sctrl_lig : 0, 
    NULL, dxs, NULL, NULL, &ssa);
  PERF_ADD_TICKS(uniscribe_analyse_ticks, mintty_perf_ticks() - perf_t0);
  if (!SUCCEEDED(hr) && hr != USP_E_SCRIPT_NOT_IN_FONT) {
    PERF_COUNT(uniscribe_analyse_failures, 1);
    use_uniscribe = false;
  }
  PERF_ADD_TICKS(text_out_start_ticks, mintty_perf_ticks() - perf_text_out_start_t0);
}

static void
text_out(HDC hdc, int x, int y, UINT fuOptions, RECT *prc, LPCWSTR psz, int cch, int *dxs)
{
  if (cch == 0)
    return;
#ifdef debug_text_out
  if (*psz >= 0x80) {
    printf("%d@%3d/%3d:", cch, x, y);
    for (int i = 0; i < cch; i++)
      printf(" %04X<%d>", psz[i], dxs[i]);
    printf("\n");
  }
#endif

  long long perf_t0 = mintty_perf_ticks();
  if (use_uniscribe) {
    PERF_COUNT(script_string_out_calls, 1);
    PERF_COUNT(script_string_out_chars, cch);
    ScriptStringOut(ssa, x, y, fuOptions, prc, 0, 0, FALSE);
    PERF_ADD_TICKS(script_string_out_ticks, mintty_perf_ticks() - perf_t0);
  }
  else {
    PERF_COUNT(ext_text_out_calls, 1);
    PERF_COUNT(ext_text_out_chars, cch);
    ExtTextOutW(hdc, x, y, fuOptions, prc, psz, cch, dxs);
    PERF_ADD_TICKS(ext_text_out_ticks, mintty_perf_ticks() - perf_t0);
  }
}

static void
text_out_end()
{
  if (use_uniscribe) {
    long long perf_t0 = mintty_perf_ticks();
    ScriptStringFree(&ssa);
    PERF_ADD_TICKS(script_string_free_ticks, mintty_perf_ticks() - perf_t0);
  }
}


typedef struct {
  HPEN pen;
  bool cached;
} selfdraw_pen_ref;

typedef struct {
  HBRUSH brush;
  bool cached;
} selfdraw_brush_ref;

typedef struct {
  bool used;
  bool ext;
  DWORD style;
  int width;
  colour col;
  HPEN pen;
} selfdraw_pen_cache_entry;

typedef struct {
  bool used;
  colour col;
  HBRUSH brush;
} selfdraw_brush_cache_entry;

#define SELFDRAW_PEN_CACHE_SIZE 512
#define SELFDRAW_BRUSH_CACHE_SIZE 256

static selfdraw_pen_cache_entry selfdraw_pen_cache[SELFDRAW_PEN_CACHE_SIZE];
static selfdraw_brush_cache_entry selfdraw_brush_cache[SELFDRAW_BRUSH_CACHE_SIZE];
static uint selfdraw_pen_cache_len;
static uint selfdraw_brush_cache_len;
static bool selfdraw_cache_overflow;
static HRGN selfdraw_clip_rgn;
static bool selfdraw_cache_registered;

static void
selfdraw_cache_cleanup(void)
{
  for (uint i = 0; i < selfdraw_pen_cache_len; i++) {
    if (selfdraw_pen_cache[i].pen)
      DeleteObject(selfdraw_pen_cache[i].pen);
    selfdraw_pen_cache[i] = (selfdraw_pen_cache_entry){0};
  }
  selfdraw_pen_cache_len = 0;
  for (uint i = 0; i < selfdraw_brush_cache_len; i++) {
    if (selfdraw_brush_cache[i].brush)
      DeleteObject(selfdraw_brush_cache[i].brush);
    selfdraw_brush_cache[i] = (selfdraw_brush_cache_entry){0};
  }
  selfdraw_brush_cache_len = 0;
  if (selfdraw_clip_rgn) {
    DeleteObject(selfdraw_clip_rgn);
    selfdraw_clip_rgn = 0;
  }
}

/*
 * If a lookup overflowed the caches since the last display update,
 * reset them so that caching resumes with the current working set.
 * Called at frame start (win_paint_buffer_begin), where no cached pen
 * or brush can be selected into any DC: every drawing path restores
 * the DC's previous objects before returning, so deleting the cached
 * objects here is safe.
 */
static void
selfdraw_cache_maintain(void)
{
  if (selfdraw_cache_overflow) {
    selfdraw_cache_cleanup();
    selfdraw_cache_overflow = false;
  }
}

static void
selfdraw_cache_register(void)
{
  if (!selfdraw_cache_registered) {
    atexit(selfdraw_cache_cleanup);
    selfdraw_cache_registered = true;
  }
}

static HPEN
selfdraw_create_pen(bool ext, DWORD style, int width, colour col)
{
  if (ext) {
    LOGBRUSH brush = (LOGBRUSH){BS_SOLID, col, 0};
    return ExtCreatePen(style, width, &brush, 0, 0);
  }
  return CreatePen(style, width, col);
}

static selfdraw_pen_ref
selfdraw_get_pen(bool ext, DWORD style, int width, colour col)
{
  selfdraw_cache_register();
  for (uint i = 0; i < selfdraw_pen_cache_len; i++) {
    selfdraw_pen_cache_entry *ent = &selfdraw_pen_cache[i];
    if (ent->used && ent->ext == ext && ent->style == style &&
        ent->width == width && ent->col == col)
      return (selfdraw_pen_ref){ent->pen, true};
  }
  if (selfdraw_pen_cache_len < lengthof(selfdraw_pen_cache)) {
    HPEN pen = selfdraw_create_pen(ext, style, width, col);
    PERF_COUNT(win_text_gdi_create_pen_calls, 1);
    if (!pen)
      return (selfdraw_pen_ref){0, false};
    selfdraw_pen_cache_entry *ent = &selfdraw_pen_cache[selfdraw_pen_cache_len++];
    *ent = (selfdraw_pen_cache_entry){true, ext, style, width, col, pen};
    return (selfdraw_pen_ref){pen, true};
  }
  /* cache full: create per call and request a reset at the next safe
     point (frame start), so a colour-rich session does not fall back
     to permanent per-call object churn */
  selfdraw_cache_overflow = true;
  HPEN pen = selfdraw_create_pen(ext, style, width, col);
  PERF_COUNT(win_text_gdi_create_pen_calls, 1);
  return (selfdraw_pen_ref){pen, false};
}

static void
selfdraw_release_pen(selfdraw_pen_ref ref)
{
  if (!ref.cached && ref.pen) {
    DeleteObject(ref.pen);
    PERF_COUNT(win_text_gdi_delete_object_calls, 1);
  }
}

static selfdraw_brush_ref
selfdraw_get_brush(colour col)
{
  selfdraw_cache_register();
  for (uint i = 0; i < selfdraw_brush_cache_len; i++) {
    selfdraw_brush_cache_entry *ent = &selfdraw_brush_cache[i];
    if (ent->used && ent->col == col)
      return (selfdraw_brush_ref){ent->brush, true};
  }
  if (selfdraw_brush_cache_len < lengthof(selfdraw_brush_cache)) {
    HBRUSH brush = CreateSolidBrush(col);
    PERF_COUNT(win_text_gdi_create_brush_calls, 1);
    if (!brush)
      return (selfdraw_brush_ref){0, false};
    selfdraw_brush_cache_entry *ent = &selfdraw_brush_cache[selfdraw_brush_cache_len++];
    *ent = (selfdraw_brush_cache_entry){true, col, brush};
    return (selfdraw_brush_ref){brush, true};
  }
  selfdraw_cache_overflow = true;  // see selfdraw_get_pen
  HBRUSH brush = CreateSolidBrush(col);
  PERF_COUNT(win_text_gdi_create_brush_calls, 1);
  return (selfdraw_brush_ref){brush, false};
}

static void
selfdraw_release_brush(selfdraw_brush_ref ref)
{
  if (!ref.cached && ref.brush) {
    DeleteObject(ref.brush);
    PERF_COUNT(win_text_gdi_delete_object_calls, 1);
  }
}

static HRGN
selfdraw_get_clip_rgn(int left, int top, int right, int bottom)
{
  selfdraw_cache_register();
  if (!selfdraw_clip_rgn) {
    selfdraw_clip_rgn = CreateRectRgn(left, top, right, bottom);
    PERF_COUNT(win_text_gdi_create_rgn_calls, 1);
  }
  else {
    SetRectRgn(selfdraw_clip_rgn, left, top, right, bottom);
  }
  return selfdraw_clip_rgn;
}

static int
perf_selfdraw_fillrect(HDC hdc, const RECT *rect, colour c)
{
  int width = rect->right - rect->left;
  int height = rect->bottom - rect->top;
  long long perf_t0 = mintty_perf_ticks();
  (void)hdc;  // fills target the global dc, see win_fill_rect
  assert(hdc == dc);
  win_fill_rect(rect, c);
  int res = 1;
  PERF_COUNT(win_text_selfdraw_fillrect_calls, 1);
  if (width > 0 && height > 0)
    PERF_COUNT(win_text_selfdraw_fillrect_pixels, (uint64_t)width * (uint64_t)height);
  PERF_ADD_TICKS(win_text_selfdraw_fillrect_ticks, mintty_perf_ticks() - perf_t0);
  return res;
}


// applies bold as colour if required, returns true if still needs thickening
static bool
old_apply_bold_colour(colour_i *pfgi)
{
  // We use two bits to control colouring and thickening for three classes of
  // colours: ANSI (0-7), default, and other (8-256, true). We also reserve one
  // combination for xterm's default (thicken everything, ANSI is also coloured).
  // - "other" is always thickened and never gets colouring.
  // - when bold_as_colour=no: ANSI+defaut are only thickened.
  // - when bold_as_colour=yes:
  //   .. and bold_as_font=no:  ANSI+default are only coloured
  //   .. and bold_as_font=yes: ANSI+default are thickened, ANSI also coloured (xterm)
  if (cfg.bold_as_colour) {
    if (CCL_ANSI8(*pfgi)) {
      *pfgi |= 8;     // (BLACK|...|WHITE)_I -> BOLD_(BLACK|...|WHITE)_I
      return cfg.bold_as_font;
    }
    if (CCL_DEFAULT(*pfgi) && !cfg.bold_as_font) {
      *pfgi |= 1;     // (FG|BG)_COLOUR_I -> BOLD_(FG|BG)_COLOUR_I
      return false;
    }
  }
  return true;
}

// removes default colours if not reversed, returns true if still needs thickening
static bool
old_rtf_bold_decolour(cattrflags attr, colour_i * pfgi, colour_i * pbgi)
{
  bool bold_thickens = cfg.bold_as_font;  // all colours
  // if not reverse:
  // - if only bold_as_colour, ATTR_BOLD still thickens default fg on default bg
  // - don't colour default fg/bg (caller interprets COLOUR_NUM as "no colour").
  if (!(attr & ATTR_REVERSE)) {
    if (cfg.bold_as_colour && CCL_DEFAULT(*pfgi) && CCL_DEFAULT(*pbgi))
      bold_thickens = true;  // even if bold_as_font=no
    if (CCL_DEFAULT(*pfgi))
      *pfgi = COLOUR_NUM;  // no colouring
    if (CCL_DEFAULT(*pbgi))
      *pbgi = COLOUR_NUM;  // no colouring
  }
  return (attr & ATTR_BOLD) && bold_thickens;
}

// Applies attributes to the fg/bg colours and returns the new cattr.
//
// "mode" maps to arbitrary sets of "things to do". Mostly these are just
// groups of attributes to handle, but it may also reflect custom needs.
//
// While {FG|BG}MASK and truefg/truebg might update, attributes remain the same.
// The exception is bold which can be applied as colour and/or thickness,
// so ATTR_BOLD bit will be turned off if further thickening should not happen.
// Always returns true colour, except ACM_RTF* modes which only do the palette.
static cattr
old_apply_attr_colour(cattr a, attr_colour_mode mode)
{
  // indexed modifications
  bool do_reverse_i = mode & (ACM_RTF_PALETTE | ACM_RTF_GEN);
  bool do_bold_i = mode & (ACM_TERM | ACM_RTF_PALETTE | ACM_RTF_GEN | ACM_SIMPLE | ACM_VBELL_BG);
#ifdef handle_blinking_here
  bool do_blink_i = mode & (ACM_TERM | ACM_RTF_PALETTE | ACM_RTF_GEN);
#endif
  bool do_finalize_rtf_i = mode & (ACM_RTF_PALETTE | ACM_RTF_GEN);
  bool do_rtf_bold_decolour_i = mode & (ACM_RTF_GEN);

  colour_i fgi = (a.attr & ATTR_FGMASK) >> ATTR_FGSHIFT;
  colour_i bgi = (a.attr & ATTR_BGMASK) >> ATTR_BGSHIFT;
  a.attr &= ~(ATTR_FGMASK | ATTR_BGMASK);  // we'll refill it later

  if (do_reverse_i && (a.attr & ATTR_REVERSE)) {
    colour_i t = fgi; fgi = bgi; bgi = t;
    colour tmp = a.truefg; a.truefg = a.truebg; a.truebg = tmp;
  }

  bool reset_bold = false;
  if (do_bold_i && (a.attr & ATTR_BOLD))    // rtf_bold_decolour uses ATTR_BOLD
    reset_bold = !old_apply_bold_colour(&fgi);  // we'll reset afterwards if needed

#ifdef handle_blinking_here
  // this is handled in term_paint
  if (do_blink_i && (a.attr & ATTR_BLINK)) {
    if (CCL_BG_ANSI8(bgi))
      bgi |= 8;
    else if (CCL_DEFAULT(bgi))
      bgi |= 1;
  }
#endif

  if (do_finalize_rtf_i) {
    if (do_rtf_bold_decolour_i) {  // uses ATTR_BOLD, ATTR_REVERSE
      bool thicken = old_rtf_bold_decolour(a.attr, &fgi, &bgi);
      if (!thicken)
        a.attr &= ~ATTR_BOLD;
    }

    if (a.attr & ATTR_INVISIBLE) {
      fgi = bgi; a.truefg = a.truebg;
    }

    a.attr |= fgi << ATTR_FGSHIFT | bgi << ATTR_BGSHIFT;
    return a;  // rtf colouring prefers indexed where possible
  }

  if (reset_bold)
    a.attr &= ~ATTR_BOLD;  // off if we should not further thicken

  // from here onward the result is true colour
  bool do_dim = mode & (ACM_TERM | ACM_SIMPLE | ACM_VBELL_BG);
  bool do_reverse = mode & (ACM_TERM);
  bool do_invisible = mode & (ACM_TERM | ACM_SIMPLE);
  bool do_vbell_bg = mode & (ACM_VBELL_BG);

  colour fg = fgi >= TRUE_COLOUR ? a.truefg : win_get_colour(fgi);
  colour bg = bgi >= TRUE_COLOUR ? a.truebg : win_get_colour(bgi);

  if (do_dim && (a.attr & ATTR_DIM)) {
    // we dim by blending fg 50-50 with the default terminal bg
    // (x & 0xFEFEFEFE) >> 1  halves each of the RGB components of x .
    // win_get_colour(..) takes term.rvideo into account.
    fg = ((fg & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
  }

  if (do_reverse && (a.attr & ATTR_REVERSE)) {
    colour t = fg; fg = bg; bg = t;
  }

  if (do_invisible && (a.attr & ATTR_INVISIBLE))
    fg = bg;

  if (do_vbell_bg)  // FIXME: we should have TATTR_VBELL. selection should too
    bg = brighten(bg, fg, false);

  // ACM_TERM does also search and cursor colours. for now we don't handle those

  a.truefg = fg;
  a.truebg = bg;
  a.attr |= TRUE_COLOUR << ATTR_FGSHIFT | TRUE_COLOUR << ATTR_BGSHIFT;
  return a;
}

// applies bold as colour if required, returns true if still needs thickening
static bool
apply_bold_colour(colour_i *pfgi)
{
  // We use two bits to control colouring and thickening for three classes of
  // colours: ANSI (0-7), default, and other (8-256, true). We also reserve one
  // combination for xterm's default (thicken everything, ANSI is also coloured).
  // - "other" is always thickened and never gets colouring.
  // - bold_as_font:  thicken ANSI/default colours.
  // - bold_as_colour: colour ANSI/default colours.
  // Exception if both false: thicken ANSI/default, colour ANSI (xterm's default).
  bool ansi = CCL_ANSI8(*pfgi);
  if (!ansi && !CCL_DEFAULT(*pfgi))
    return true;  // neither ANSI nor default -> always thicken, no colouring

  if (!cfg.bold_as_colour && !cfg.bold_as_font) {  // the exception: xterm-like
    if (ansi)  // coloured
      *pfgi |= 8;  // (BLACK|...|WHITE)_I -> BOLD_(BLACK|...|WHITE)_I
    return true;  // both thickened
  }
  // switchable attribute colours
  if (term.enable_bold_colour && CCL_DEFAULT(*pfgi)
      && colours[BOLD_COLOUR_I] != (colour)-1
     )
    *pfgi = BOLD_COLOUR_I;
  else if (term.enable_blink_colour && CCL_DEFAULT(*pfgi)
      && colours[BLINK_COLOUR_I] != (colour)-1
     )
    *pfgi = BLINK_COLOUR_I;
  else
  // normal independent as_font/as_colour controls
  if (cfg.bold_as_colour) {
    if (ansi)
      *pfgi |= 8;
    else  // default
      *pfgi |= 1;  // (FG|BG)_COLOUR_I -> BOLD_(FG|BG)_COLOUR_I
  }
  return cfg.bold_as_font;  // thicken if bold_as_font
}

// removes default colours if not reversed, returns true if still needs thickening
static bool
rtf_bold_decolour(cattrflags attr, colour_i * pfgi, colour_i * pbgi)
{
  bool bold_thickens = cfg.bold_as_font;  // all colours
  // if not reverse:
  // - ATTR_BOLD always thickens default fg on default bg
  // - don't colour default fg/bg (caller interprets COLOUR_NUM as "no colour").
  if (!(attr & ATTR_REVERSE)) {
    if (CCL_DEFAULT(*pfgi) && CCL_DEFAULT(*pbgi))
      bold_thickens = true;  // even if bold_as_font=no
    if (CCL_DEFAULT(*pfgi))
      *pfgi = COLOUR_NUM;  // no colouring
    if (CCL_DEFAULT(*pbgi))
      *pbgi = COLOUR_NUM;  // no colouring
  }
  return (attr & ATTR_BOLD) && bold_thickens;
}

// Applies attributes to the fg/bg colours and returns the new cattr.
//
// "mode" maps to arbitrary sets of "things to do". Mostly these are just
// groups of attributes to handle, but it may also reflect custom needs.
//
// While {FG|BG}MASK and truefg/truebg might update, attributes remain the same.
// The exception is bold which can be applied as colour and/or thickness,
// so ATTR_BOLD bit will be turned off if further thickening should not happen.
// Always returns true colour, except ACM_RTF* modes which only do the palette.
cattr
apply_attr_colour(cattr a, attr_colour_mode mode)
{
  if (cfg.old_bold)
    return old_apply_attr_colour(a, mode);

  // indexed modifications
  bool do_reverse_i = mode & (ACM_RTF_PALETTE | ACM_RTF_GEN);
  bool do_bold_i = mode & (ACM_TERM | ACM_RTF_PALETTE | ACM_RTF_GEN | ACM_SIMPLE | ACM_VBELL_BG);
#ifdef handle_blinking_here
  bool do_blink_i = mode & (ACM_TERM | ACM_RTF_PALETTE | ACM_RTF_GEN);
#endif
  bool do_finalize_rtf_i = mode & (ACM_RTF_PALETTE | ACM_RTF_GEN);
  bool do_rtf_bold_decolour_i = mode & (ACM_RTF_GEN);

  colour_i fgi = (a.attr & ATTR_FGMASK) >> ATTR_FGSHIFT;
  colour_i bgi = (a.attr & ATTR_BGMASK) >> ATTR_BGSHIFT;
  a.attr &= ~(ATTR_FGMASK | ATTR_BGMASK);  // we'll refill it later

  if (do_reverse_i && (a.attr & ATTR_REVERSE)) {
    colour_i t = fgi; fgi = bgi; bgi = t;
    colour tmp = a.truefg; a.truefg = a.truebg; a.truebg = tmp;
  }

  bool reset_bold = false;
  if (do_bold_i && (a.attr & ATTR_BOLD))    // rtf_bold_decolour uses ATTR_BOLD
    reset_bold = !apply_bold_colour(&fgi);  // we'll reset afterwards if needed

#ifdef handle_blinking_here
  // this is handled in term_paint
  if (do_blink_i && (a.attr & ATTR_BLINK)) {
    if (CCL_BG_ANSI8(bgi))
      bgi |= 8;
    else if (CCL_DEFAULT(bgi))
      bgi |= 1;
  }
#endif

  if (do_finalize_rtf_i) {
    if (do_rtf_bold_decolour_i) {  // uses ATTR_BOLD, ATTR_REVERSE
      bool thicken = rtf_bold_decolour(a.attr, &fgi, &bgi);
      if (!thicken)
        a.attr &= ~ATTR_BOLD;
    }

    if (a.attr & ATTR_INVISIBLE) {
      fgi = bgi; a.truefg = a.truebg;
    }

    a.attr |= fgi << ATTR_FGSHIFT | bgi << ATTR_BGSHIFT;
    return a;  // rtf colouring prefers indexed where possible
  }

  if (reset_bold)
    a.attr &= ~ATTR_BOLD;  // off if we should not further thicken

  // from here onward the result is true colour
  bool do_dim = mode & (ACM_TERM | ACM_SIMPLE | ACM_VBELL_BG);
  bool do_reverse = mode & (ACM_TERM);
  bool do_invisible = mode & (ACM_TERM | ACM_SIMPLE);
  bool do_vbell_bg = mode & (ACM_VBELL_BG);

  colour fg = fgi >= TRUE_COLOUR ? a.truefg : win_get_colour(fgi);
  colour bg = bgi >= TRUE_COLOUR ? a.truebg : win_get_colour(bgi);

  if (do_dim && (a.attr & ATTR_DIM)) {
    // we dim by blending fg 50-50 with the default terminal bg
    // (x & 0xFEFEFEFE) >> 1  halves each of the RGB components of x .
    // win_get_colour(..) takes term.rvideo into account.
    fg = ((fg & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
  }

  if (do_reverse && (a.attr & ATTR_REVERSE)) {
    colour t = fg; fg = bg; bg = t;
  }

  if (do_invisible && (a.attr & ATTR_INVISIBLE))
    fg = bg;

  if (do_vbell_bg)  // FIXME: we should have TATTR_VBELL. selection should too
    bg = brighten(bg, fg, false);

  // ACM_TERM does also search and cursor colours. for now we don't handle those

  if (a.attr & TATTR_CLEAR)
    bg = brighten(bg, fg, false);

  a.truefg = fg;
  a.truebg = bg;
  a.attr |= TRUE_COLOUR << ATTR_FGSHIFT | TRUE_COLOUR << ATTR_BGSHIFT;
  return a;
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
   clearpad: flag to clear padding from overhang
   phase: overlay line display (italic right-to-left overhang handling)
 */
void
win_text(int tx, int ty, wchar *text, int len, cattr attr, cattr *textattr, ushort lattr, char has_rtl, char has_sea, bool clearpad, uchar phase)
{
  long long perf_win_text_start = mintty_perf_ticks();
  PERF_COUNT(win_text_calls, 1);
  PERF_COUNT(win_text_chars, len);
  if (phase == 0)
    PERF_COUNT(win_text_phase0_calls, 1);
  else if (phase == 1)
    PERF_COUNT(win_text_phase1_calls, 1);
  else if (phase == 2)
    PERF_COUNT(win_text_phase2_calls, 1);
  if (clearpad)
    PERF_COUNT(win_text_clearpad_calls, 1);
  for (int perf_i = 0; perf_i < len; perf_i++) {
    if (text[perf_i] >= ' ' && text[perf_i] <= '~')
      PERF_COUNT(win_text_ascii_chars, 1);
    else
      PERF_COUNT(win_text_nonascii_chars, 1);
  }

  if (paint_buffered) {
    // extend the dirty row span for the back buffer blit;
    // recording unconditionally can only over-extend the span
    // (a blit of unchanged rows is loss-free), never miss painting
    if (ty < paint_dirty_top) {
      paint_dirty_top = ty;
    }
    if (ty > paint_dirty_bot) {
      paint_dirty_bot = ty;
    }
  }
#ifdef debug_wscale
  if (attr.attr & (TATTR_EXPAND | TATTR_NARROW | TATTR_WIDE))
    for (int i = 0; i < len; i++)
      printf("[%2d:%2d] %c%c%c%c %04X\n", ty, tx + i, attr.attr & TATTR_NARROW ? 'n' : ' ', attr.attr & TATTR_EXPAND ? 'x' : ' ', attr.attr & TATTR_WIDE ? 'w' : ' ', " WUL"[lattr & LATTR_MODE], text[i]);
#endif
  //if (kb_trace) {printf("[%ld] <win_text\n", mtime()); kb_trace = 0;}

  long long perf_font_resolve_t0 = mintty_perf_ticks();
  int findex = (attr.attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  bool boxpower = false;  // Box Drawing or Powerline symbols
  bool boxcoded = false;  // coded DEC box drawing and scanlines
  bool vt52fraction = false;
  bool dectcs = false;
  // self-drawn graphic glyphs and some special graphic handling are 
  // indicated as unused font family number, in order to save attribute bits
  //  11    Unicode Box Drawing and Powerline box drawing
  //  12    encoded VT100 box drawing and VT100/VT52 scanlines
  //  13    VT52 fraction numerators 3/ 5/ 7/
  //  14    DEC Technical Character Set square root, sum segments, triangles
  if (findex > 10) {
    if (findex == 11)  // Unicode Box Drawing and Powerline box drawing
      boxpower = true;
    else if (findex == 12) // VT100 box drawing, VT100/VT52 scanlines
      boxcoded = true;
    else if (findex == 13) // VT52 fraction numerators
      vt52fraction = true;
    else if (findex == 14) // DEC Technical Character Set (TCS)
      dectcs = true;

    findex = 0;
  }
  if (boxpower) {
    PERF_COUNT(win_text_boxpower_calls, 1);
    PERF_COUNT(win_text_boxpower_chars, len);
  }
  if (boxcoded) {
    PERF_COUNT(win_text_boxcoded_calls, 1);
    PERF_COUNT(win_text_boxcoded_chars, len);
  }
  if (vt52fraction) {
    PERF_COUNT(win_text_vt52fraction_calls, 1);
    PERF_COUNT(win_text_vt52fraction_chars, len);
  }
  if (dectcs) {
    PERF_COUNT(win_text_dectcs_calls, 1);
    PERF_COUNT(win_text_dectcs_chars, len);
  }
  if (has_rtl)
    PERF_COUNT(win_text_rtl_calls, 1);
  if (has_sea)
    PERF_COUNT(win_text_sea_calls, 1);

  struct fontfam * ff = &fontfamilies[findex];
  // check whether font lacks support of given RTL bidi class
  // has_rtl:    1: R/Hebrew,    2: AL/Arabic,    4: other
  // ff->no_rtl: 1: no R/Hebrew, 2: no AL/Arabic, 4: either
  if (has_rtl & ff->no_rtl) {
    //printf("%d<%ls> %X %X\n", findex, ff->name, has_rtl, ff->no_rtl);
    // for RTL output, if font does not support RTL, 
    // fallback to reserved font 11 (configured via FontRTL)
    findex = 11;
    ff = &fontfamilies[findex];
  }

  // set horizontal shift if script-triggered to center CJK (#1313)
#ifdef configured_glyph_shift
  int glyph_shift = (attr.attr & GLYPHSHIFT_MASK) >> ATTR_GLYPHSHIFT_SHIFT;
#else
  int glyph_shift = (attr.attr & ATTR_GLYPHSHIFT) ? ff->shift : 0;
#endif

  trace_line("win_text:");

  bool ldisp1 = phase == 1;
  bool ldisp2 = phase == 2;
  bool lpresrtl = lattr & LATTR_PRESRTL;
  lattr &= LATTR_MODE;

  int char_width = cell_width * (1 + (lattr != LATTR_NORM));

 /* Only want the left half of double width lines */
  // check this before scaling up x to pixels!
  if (lattr != LATTR_NORM && tx * 2 >= term.cols) {
    PERF_ADD_TICKS(win_text_ticks, mintty_perf_ticks() - perf_win_text_start);
    return;
  }

 /* Convert to window coordinates */
  int x = tx * char_width + PADDING;
  int y = ty * cell_height + OFFSET + PADDING;

#ifdef support_triple_width
#define TATTR_TRIPLE 0x0080000000000000u
  if ((attr.attr & TATTR_TRIPLE) == TATTR_TRIPLE) {
    char_width *= 3;
    attr.attr &= ~TATTR_TRIPLE;
  }
  else
#endif
  if (attr.attr & TATTR_WIDE)
    char_width *= 2;

  bool wscale_narrow_50 = false;
  if ((attr.attr & (TATTR_NARROW | TATTR_CLEAR)) == (TATTR_NARROW | TATTR_CLEAR)) {
    // indicator for adjustment of auto-narrowing;
    // geometric Powerline symbols, explicit single-width attribute narrowing
    attr.attr &= ~TATTR_CLEAR;
    wscale_narrow_50 = true;
  }

  bool dim_font = ff->dim_mode == DIM_FONT && attr.attr & ATTR_DIM;

  bool default_bg = (attr.attr & ATTR_BGMASK) >> ATTR_BGSHIFT == BG_COLOUR_I;
  if (attr.attr & ATTR_REVERSE)
    default_bg = false;
  //cattr attr0 = attr;  // needed unmodified colour attributes for combinings
  long long perf_attr_colour_t0 = mintty_perf_ticks();
  attr = apply_attr_colour(attr, ACM_TERM);
  PERF_ADD_TICKS(win_text_attr_colour_ticks, mintty_perf_ticks() - perf_attr_colour_t0);
  colour fg = attr.truefg;
  colour bg = attr.truebg;
  // ATTR_BOLD is now set if and only if we need further thickening.

  bool has_cursor = attr.attr & (TATTR_ACTCURS | TATTR_PASCURS);
  colour cursor_colour = 0;

#ifdef keep_sel_colour_here
  if ((attr.attr & ATTR_BGMASK) >> ATTR_BGSHIFT == SEL_COLOUR_I)
#else
  if (attr.attr & TATTR_SELECTED)
#endif
    default_bg = false;

  if (attr.attr & (TATTR_CURRESULT | TATTR_CURMARKED)) {
    bg = cfg.search_current_colour;
    fg = cfg.search_fg_colour;
    default_bg = false;
  }
  else if (attr.attr & (TATTR_RESULT | TATTR_MARKED)) {
    bg = cfg.search_bg_colour;
    fg = cfg.search_fg_colour;
    default_bg = false;
  }

 /* Suppress graphic background at cursor position */
  if (has_cursor)
    if (term_cursor_type() == CUR_BLOCK && (attr.attr & TATTR_ACTCURS))
      default_bg = false;

 /* Cursor contrast adjustment for background or block cursor */
  if (has_cursor && (phase < 2 || term_cursor_type() == CUR_BLOCK)) {
    // To extend this heuristics to other cursor styles, 
    // some tricky interworking needs to be sorted out (#1157);
    // currently the assumption is that line cursors should be thin enough 
    // to make this fix less important
    cursor_colour = colours[ime_open ? IME_CURSOR_COLOUR_I : CURSOR_COLOUR_I];
    //printf("cc (ime_open %d) %06X\n", ime_open, cursor_colour);

    //static uint mindist = 32768;
    static uint mindist = 22222;
    //static uint mindist = 8000;
    bool too_close = colour_dist(cursor_colour, bg) < mindist;

    if (too_close) {
      //cursor_colour = fg;
      colour ccfg = brighten(cursor_colour, fg, false);
      colour ccbg = brighten(cursor_colour, bg, false);
      if (colour_dist(ccfg, bg) < mindist
          && colour_dist(ccfg, bg) < colour_dist(ccbg, bg)
         )
        cursor_colour = ccbg;
      else
        cursor_colour = ccfg;
    }

    if ((attr.attr & TATTR_ACTCURS) && term_cursor_type() == CUR_BLOCK) {
      fg = colours[CURSOR_TEXT_COLOUR_I];
      if (too_close && colour_dist(cursor_colour, fg) < mindist)
        fg = bg;
      bg = cursor_colour;
#ifdef debug_cursor
      printf("set cursor (colour %06X) @(row %d col %d) cursor_on %d\n", bg, (y - PADDING - OFFSET) / cell_height, (x - PADDING) / char_width, term.cursor_on);
#endif
    }
  }

 /* Now that attributes are (almost) sorted out, select proper font */
  uint nfont;
  switch (lattr) {
    when LATTR_NORM: nfont = 0;
    when LATTR_WIDE: nfont = FONT_WIDE;
    otherwise:       nfont = FONT_WIDE + FONT_HIGH;
  }

  if (dim_font)
    nfont |= FONT_DIM;

  int wscale = 100;

  if (attr.attr & TATTR_EXPAND) {
    if (nfont & FONT_WIDE)
      wscale = 200;
    nfont |= FONT_WIDE;
  }
  else if (wscale_narrow_50)
    wscale = 50;
#ifndef narrow_via_font
  else if ((attr.attr & TATTR_NARROW) && !(attr.attr & TATTR_ZOOMFULL)) {
    wscale = cfg.char_narrowing;
    if (wscale > 100)
      wscale = 100;
    if (wscale < 50)
      wscale = 50;
    nfont |= FONT_NARROW;
  }
#endif

  bool do_special_underlay = false;
  if (cfg.bold_as_special && (attr.attr & ATTR_BOLD)) {
    do_special_underlay = true;
    attr.attr &= ~ATTR_BOLD;
  }
  if (ff->bold_mode == BOLD_FONT && (attr.attr & ATTR_BOLD))
    nfont |= FONT_BOLD;
  if (ff->und_mode == UND_FONT && (attr.attr & UNDER_MASK) == ATTR_UNDER
      && !(attr.attr & ATTR_ULCOLOUR)
     )
    nfont |= FONT_UNDERLINE;
  if (attr.attr & ATTR_ITALIC)
    nfont |= FONT_ITALIC;
  if (attr.attr & ATTR_STRIKEOUT
      && !cfg.underl_manual && cfg.underl_colour == (colour)-1
      && !(attr.attr & ATTR_ULCOLOUR)
     )
    nfont |= FONT_STRIKEOUT;
  if (attr.attr & TATTR_ZOOMFULL)
    nfont |= FONT_ZOOMFULL;
  if (attr.attr & (ATTR_SUBSCR | ATTR_SUPERSCR))
    nfont |= FONT_ZOOMSMALL;
  if (attr.attr & TATTR_SINGLE)
    nfont |= FONT_ZOOMDOWN;
  another_font(ff, nfont);

  bool force_manual_underline = false;
  if (!ff->fonts[nfont]) {
    if (nfont & FONT_UNDERLINE)
      force_manual_underline = true;
    // Don't force manual bold, it could be bad news.
    nfont &= ~(FONT_BOLD | FONT_UNDERLINE);
  }
#ifdef narrow_via_font
  if ((nfont & (FONT_WIDE | FONT_NARROW)) == (FONT_WIDE | FONT_NARROW))
    nfont &= ~(FONT_WIDE | FONT_NARROW);
#endif

  another_font(ff, nfont);
  if (!ff->fonts[nfont])
    nfont = FONT_NORMAL;

#if defined(debug_bold) && debug_bold > 1
  wchar t[len + 1]; wcsncpy(t, text, len); t[len] = 0;
  printf("font %02X (%dpt) bold_mode %d attr_bold %d fg %06X <%ls>\n", nfont, font_size, ff->bold_mode, !!(attr.attr & ATTR_BOLD), fg, t);
#endif

  PERF_ADD_TICKS(win_text_font_resolve_ticks, mintty_perf_ticks() - perf_font_resolve_t0);

 /* With selected font, begin preparing the rendering */
  long long perf_state_t0 = mintty_perf_ticks();
 /* Self-drawn graphics runs (origtext will be set below) render no
    glyphs through the run font - they take the skip_drawing path and
    draw with pens and fills - so selecting it is a wasted kernel
    transition on the DIB-backed DC. Worse, the text/box alternation
    of line-art screens defeats the same-font skip of win_select_font
    for the interleaved text runs. Skip the selection for such runs;
    the special underlay glyph, the one glyph output a self-drawn run
    can still make, selects its font on the spot. vt52 fraction runs
    render through the normal text path and keep the selection. */
  bool selfdrawn_run = boxpower || boxcoded || dectcs;
  if (!selfdrawn_run) {
    win_select_font(ff->fonts[nfont]);
    PERF_COUNT(select_font_calls, 1);
  }
  PERF_ADD_TICKS(select_font_ticks, mintty_perf_ticks() - perf_state_t0);
  perf_state_t0 = mintty_perf_ticks();
  SetTextColor(dc, fg);
  PERF_COUNT(set_text_color_calls, 1);
  PERF_ADD_TICKS(set_text_color_ticks, mintty_perf_ticks() - perf_state_t0);
  perf_state_t0 = mintty_perf_ticks();
  SetBkColor(dc, bg);
  PERF_COUNT(set_bk_color_calls, 1);
  PERF_ADD_TICKS(set_bk_color_ticks, mintty_perf_ticks() - perf_state_t0);

#define dont_debug_missing_glyphs
#ifdef debug_missing_glyphs
  ushort glyph[len];
  GetGlyphIndicesW(dc, text, len, glyph, true);
  for (int i = 0; i < len; i++)
    if (glyph[i] == 0xFFFF)
      printf(" %04X -> no glyph\n", text[i]);
#endif

 /* Check whether the text has any right-to-left characters */
#ifdef check_rtl_here
#warning now passed as a parameter to avoid redundant checking
  bool has_rtl = false;
  for (int i = 0; i < len && !has_rtl; i++)
    has_rtl = is_rtl(text[i]);
#endif

  uint eto_options = ETO_CLIPPED;
#ifdef glyph_output
  // this mode of output processing with unclear purpose used to corrupt
  // cursor and italic display of right-to-left scripts; disabled
  if (has_rtl) {
   /* We've already done right-to-left processing in the screen buffer,
    * so stop Windows from doing it again (and hence undoing our work).
    * Don't always use this path because GetCharacterPlacement doesn't
    * do Windows font linking.
    */
    char classes[len];
    memset(classes, GCPCLASS_NEUTRAL, len);

    GCP_RESULTSW gcpr = {
      .lStructSize = sizeof(GCP_RESULTSW),
      .lpClass = (void *)classes,
      .lpGlyphs = text,
      .nGlyphs = len
    };

    trace_line(" <ChrPlc:");
    // This does not work for non-BMP:
    GetCharacterPlacementW(dc, text, len, 0, &gcpr,
                           FLI_MASK | GCP_CLASSIN | GCP_DIACRITIC);
    len = gcpr.nGlyphs;
    trace_line(" >ChrPlc:");
    eto_options |= ETO_GLYPH_INDEX;
  }
#endif


  bool combining = attr.attr & TATTR_COMBINING;
  bool combining_double = attr.attr & TATTR_COMBDOUBL;
  if (combining)
    PERF_COUNT(win_text_combining_calls, 1);
  if (combining_double)
    PERF_COUNT(win_text_combining_double_calls, 1);

  bool let_windows_combine = false;
  if (combining) {
#ifdef substitute_combining_chars
   /* Substitute combining characters by overprinting lookalike glyphs */
   /* Dropped for unpleasant effects, see comments at function combsubst */
    for (int i = 0; i < len; i++)
      text[i] = combsubst(text[i], attr.attr);
#endif
   /* Determine characters that should be combined by Windows */
    if (len == 2) {
      if (text[0] == 'i' && (text[1] == 0x030F || text[1] == 0x0311))
        let_windows_combine = true;
     /* Enforce separate combining characters display if colours differ */
      if (!termattrs_equal_fg(&textattr[1], &attr))
        let_windows_combine = false;
    }
  }

  wchar * origtext = 0;
  if (boxpower || boxcoded || dectcs) {
    /* Self-drawn graphics skip the normal text-output path below. Keep a
       pointer to the original run without allocating a dummy text buffer. */
    origtext = text;
  }

 /* Array with offsets between neighbouring characters */
  int dxs[len];
  long long perf_dxs_t0 = mintty_perf_ticks();
  int dx = combining ? 0 : char_width;
  for (int i = 0; i < len; i++) {
    if (is_high_surrogate(text[i]))
      // This does not have the expected effect so we keep splitting up 
      // non-BMP characters into single character chunks for now (term.c)
      dxs[i] = 0;
    else
      dxs[i] = dx;
  }
  PERF_ADD_TICKS(win_text_dxs_ticks, mintty_perf_ticks() - perf_dxs_t0);

 /* Character cells length */
  long long perf_ulen_t0 = mintty_perf_ticks();
  int ulen = 0;
  for (int i = 0; i < len; i++) {
    ulen++;
    if (char1ulen(&text[i]) == 2)
      i++;  // skip low surrogate;
  }
  PERF_ADD_TICKS(win_text_ulen_ticks, mintty_perf_ticks() - perf_ulen_t0);

 /* Painting box */
  int width = char_width * (combining ? 1 : ulen);
  RECT box = {
    .top = y, .bottom = y + cell_height,
    .left = x, .right = min(x + width, cell_width * term.cols + PADDING)
  };
  // extend bounding box for appended composed combining characters
  if (has_sea & 2)
    box.right += cell_width;
  RECT box0 = box;
  if (ldisp2) {  // e.g. attr.attr & ATTR_ITALIC
    box.right += cell_width;
    box.left -= cell_width;
  }
  if (clearpad && tx > 0)
    box.right += PADDING;
  RECT box2 = box;
  if (combining_double)
    box2.right += char_width;


 /* Uniscribe handling */
  long long perf_uniscribe_decision_t0 = mintty_perf_ticks();
  use_uniscribe = cfg.font_render == FR_UNISCRIBE && !has_rtl;
  if (combining_double)
    use_uniscribe = false;
  if (use_uniscribe) {
   /* Skip Uniscribe shaping for runs consisting entirely of blanks.
      Such runs are frequent (indentation, cleared regions, line tails,
      blanked SIXEL areas) and per-run ScriptStringAnalyse is the
      dominant fixed cost of text output on large windows. Shaping
      cannot alter a run of U+0020: no ligatures or fallback apply, and
      the plain ExtTextOutW path paints the identical background fill
      and font-based underline/strikeout for the same cell advances. */
    bool blank_run = true;
    for (int i = 0; i < len && blank_run; i++) {
      blank_run = text[i] == ' ';
    }
    if (blank_run) {
      use_uniscribe = false;
    }
  }
  if (use_uniscribe && cfg.ligatures == 0 && findex == 0) {
   /* With ligature application disabled (Ligatures=0), skip Uniscribe
      shaping for runs consisting entirely of printable ASCII in the
      primary font. For such runs, shaping can only differ from plain
      ExtTextOutW output through default OpenType substitutions
      (liga/calt programming ligatures), which this setting disavows;
      cell advances are forced by dxs either way. This avoids the
      per-run ScriptStringAnalyse fixed cost for the bulk of typical
      terminal content. The bypass is restricted to the primary font
      so that alternative fonts keep Uniscribe font fallback even for
      ASCII; RTL runs never get here (use_uniscribe already false)
      and runs with combining characters contain non-ASCII text. */
    bool ascii_run = true;
    for (int i = 0; i < len && ascii_run; i++) {
      ascii_run = text[i] >= ' ' && text[i] <= '~';
    }
    if (ascii_run) {
      use_uniscribe = false;
    }
  }
#ifdef no_Uniscribe_for_ASCII_only_chunks
  // this "optimization" was intended to avoid a performance penalty 
  // for Uniscribe when there is no need for Uniscribe;
  // however, even for ASCII-only chunks, 
  // Uniscribe effectively applies ligatures (Fira Code, #601), 
  // and testing again, there is hardly a penalty observable anymore
  if (use_uniscribe) {
    use_uniscribe = false;
    for (int i = 0; i < len; i++)
      if (text[i] >= 0x80) {
        use_uniscribe = true;
        break;
      }
  }
#endif
  PERF_ADD_TICKS(win_text_uniscribe_decision_ticks, mintty_perf_ticks() - perf_uniscribe_decision_t0);

#ifdef debug_non_blank_lines
  void printline() {
    bool dopri = false;
    for (int i = 0; i < len; i++)
      if (text[i] != ' ') {
        dopri = true;
        break;
      }
    if (dopri) {
      printf("%d:%d %06X %06X (def %d) %d", ty, tx, fg, bg, default_bg, len);
      for (int i = 0; i < len; i++)
        printf(" %02X", text[i]);
      printf("\n");
    }
  }
  printline();
#endif

 /* Begin text output */
  int yt = y + (ff->row_spacing / 2) - (lattr == LATTR_BOT ? cell_height : 0);
  int xt = x + (ff->col_spacing / 2);
  if (attr.attr & TATTR_ZOOMFULL) {
    yt -= ff->row_spacing / 2;
    xt = x;
  }

  int line_width = (3
                    + (attr.attr & ATTR_BOLD ? 1 : 0)
                    + (lattr >= LATTR_WIDE ? 2 : 0)
                    + (lattr >= LATTR_TOP ? 2 : 0)
                   ) * cell_height / 40;
  if (line_width < 1)
    line_width = 1;

 /* Determine shadow/overstrike bold or double-width/height width */
  int xwidth = 1;
  if (ff->bold_mode == BOLD_SHADOW && (attr.attr & ATTR_BOLD)) {
    // This could be scaled with font size, but at risk of clipping
    xwidth = 2;
    if (lattr != LATTR_NORM) {
      xwidth = 3; // 4?
    }
  }

 /* Manual underline */
  colour ul = fg;
  int uloff = ff->descent + (cell_height - ff->descent + 1) / 2;
  if (lattr == LATTR_BOT)
    uloff = ff->descent + (cell_height - ff->descent + 1) / 2;
  uloff += line_width / 2;
  if (uloff >= cell_height)
    uloff = cell_height - 1;

  if (attr.attr & ATTR_ULCOLOUR)
    ul = attr.ulcolr;
  else if (cfg.underl_colour != (colour)-1)
    ul = cfg.underl_colour;
#ifdef debug_underline
  if (cfg.underl_colour == (colour)-1)
    ul = 0x802020E0;
  if (lattr == LATTR_TOP)
    ul = 0x80E0E020;
  if (lattr == LATTR_BOT)
    ul = 0x80E02020;
#endif
#ifdef debug_bold
  if (xwidth > 1) {
    force_manual_underline = true;
    ul = 0x802020E0;
  }
  else if (nfont & FONT_BOLD) {
    force_manual_underline = true;
    ul = 0x8020E020;
  }
#endif

  bool underlaid = false;
  void clear_run() {
    if (!underlaid) {
      // clear background of current output chunk;
      // the recolourable stock DC brush yields the identical solid fill
      // while avoiding CreateSolidBrush/DeleteObject GDI object churn
      // on every painted run
      int perf_fill_w = box.right - box.left;
      int perf_fill_h = box.bottom - box.top;
      long long perf_fill_t0 = mintty_perf_ticks();
      win_fill_rect(&box, bg);
      PERF_COUNT(fillrect_calls, 1);
      if (perf_fill_w > 0 && perf_fill_h > 0)
        PERF_COUNT(fillrect_pixels, (uint64_t)perf_fill_w * (uint64_t)perf_fill_h);
      PERF_ADD_TICKS(fillrect_ticks, mintty_perf_ticks() - perf_fill_t0);

      underlaid = true;
    }
  }

 /* Graphic background: picture or texture */
  long long perf_background_t0 = mintty_perf_ticks();
  if (*cfg.background && default_bg) {
    RECT bgbox = box0;

    // extend into padding area
    if (!tx)
      bgbox.left = 0;
    if (bgbox.right >= PADDING + cell_width * term.cols)
      bgbox.right += PADDING;
    if (!ty)
      bgbox.top = 0;
    if (ty == term.rows - 1) {
      RECT cr;
      GetClientRect(wnd, &cr);
      if (win_search_visible())
        cr.bottom -= SEARCHBAR_HEIGHT;
      bgbox.bottom = cr.bottom;
    }

    if (fill_background(dc, &bgbox))
      underlaid = true;
#ifdef debug_text_background
    if (*text != 'x')
      underlaid = false;
#endif
  }
  else if (origtext && !ldisp2)
    clear_run();  // clear background for self-drawn characters (#1310)
  PERF_ADD_TICKS(win_text_background_ticks, mintty_perf_ticks() - perf_background_t0);

 /* Coordinate transformation per line */
  long long perf_coord_line_t0 = mintty_perf_ticks();
  int coord_transformed = 0;
  XFORM old_xform;
  if (lpresrtl) {
    coord_transformed = SetGraphicsMode(dc, GM_ADVANCED);
    if (coord_transformed && GetWorldTransform(dc, &old_xform)) {
      XFORM xform = (XFORM){-1.0, 0.0, 0.0, 1.0, term.cols * cell_width + 2 * PADDING, 0.0};
      coord_transformed = SetWorldTransform(dc, &xform);
      if (coord_transformed) {
        paint_dc_busy_push();  // popped at the coord_transformed restore
      }
    }
  }
  PERF_ADD_TICKS(win_text_coord_line_ticks, mintty_perf_ticks() - perf_coord_line_t0);

 /* Special underlay */
  if (do_special_underlay && !ldisp2) {
    // ensure the run font for the underlay glyph; self-drawn runs skip
    // the selection above, and for text runs this is a cache hit
    win_select_font(ff->fonts[nfont]);
    xchar uc = 0x2312;
    int ulaylen = uc > 0xFFFF ? ulen * 2 : ulen;
    wchar ulay[ulaylen];
    for (int i = 0; i < ulaylen; i++)
      if (uc > 0xFFFF)
        if (i & 1)
          ulay[i] = low_surrogate(uc);
        else
          ulay[i] = high_surrogate(uc);
      else
        ulay[i] = uc;

    static colour rainbow[] = { // https://en.wikipedia.org/wiki/Rainbow
      RGB(0xFF, 0x00, 0x00), // red
      RGB(0xFF, 0x66, 0x00), // orange
      RGB(0xFF, 0xEE, 0x00), // yellow
      RGB(0x00, 0xFF, 0x00), // green
      RGB(0x00, 0x99, 0xFF), // blue
      RGB(0x44, 0x00, 0xFF), // indigo
      RGB(0x99, 0x00, 0xFF), // violet
      };

    int dist = cell_height / 20 + 1;
    int leap = cell_height <= 20 ? 3 : cell_height <= 30 ? 2 : 1;
    int y = yt - cell_height / 2 + 4 * dist;
    for (uint c = 0; c < lengthof(rainbow); c += leap) {
      SetTextColor(dc, rainbow[c]);
      uint eto = eto_options;
      if (!underlaid && !c)
        eto |= ETO_OPAQUE;
      ExtTextOutW(dc, xt, y, eto, &box, ulay, ulaylen, dxs);
      y += dist;
    }
    SetTextColor(dc, bg);
    ExtTextOutW(dc, xt, y, eto_options, &box, ulay, ulaylen, dxs);
    SetTextColor(dc, fg);

    underlaid = true;
  }

  if (attr.attr & (ATTR_SUBSCR | ATTR_SUPERSCR)) {
    xt += cell_width * 3 / 10;
    switch (attr.attr & (ATTR_SUBSCR | ATTR_SUPERSCR)) {
      when ATTR_SUBSCR | ATTR_SUPERSCR:
        yt += cell_height * 10 / 32;  // 11 fits better but closer to subscr
      when ATTR_SUBSCR:
        yt += cell_height * 13 / 32;  // 14 looks better but at some clipping
      when ATTR_SUPERSCR:
        yt += cell_height * 1 / 8;
    }
  }
  if (attr.attr & TATTR_SINGLE)
    yt += cell_height / 4;

 /* Shadow */
  int layer = 0;
  colour fg0 = fg;
  colour ul0 = ul;
  if (attr.attr & ATTR_SHADOW) {
    layer = 1;
    xt += line_width;
    yt -= layer * line_width;
    x += line_width;
    y -= layer * line_width;
    fg = ((fg & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
    ul = ((ul & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
    SetTextColor(dc, fg);
  }

draw:;
#ifdef debug_draw
  if (*text != ' ')
    printf("draw @%d:%d %d:%d %d:%d\n", ty, tx, yt, xt, y, x);
#endif
  int yt0 = yt;
  int xt0 = xt;
  int y0 = y;
  int x0 = x;

 /* Wavy underline */
  if (!ldisp2 && lattr != LATTR_TOP &&
      (attr.attr & UNDER_MASK) == ATTR_CURLYUND
     )
  {
    clear_run();
    //printf("curly %d:%d %d:%d (w %d ulen %d cw %d)\n", ty, tx, y, x, width, ulen, char_width);

    int step = 4;  // horizontal step width
    int delta = 3; // vertical phase height
    int offset = 1; // offset up from uloff
    //int x0 = x - PADDING - (x - PADDING) % step + PADDING;
    //int rep = (ulen * char_width - 1) / step / 3 + 2;
    // when starting the undercurl at the current text start position,
    // the transitions between adjacent Bezier waves looks disrupted;
    // trying to align them by snapping to a previous point (- x... % step)
    // does not help, maybe Bezier curves should be studied first;
    // so in order to achieve uniform undercurls, we always start at 
    // the line beginning position (getting clipped anyway)
    int x0 = PADDING;
    int rep = (x - x0 + width + char_width) / step / 3;

    POINT bezier[1 + rep * 3];
    //printf("                      ");
    for (int i = 0; i <= rep * 3; i++) {
      bezier[i].x = x0 + i * step;
      int wave = (i % 3 == 2) ? delta : (i % 3 == 1) ? -delta : 0;
      bezier[i].y = y + uloff - offset + wave;
      //printf("  %04d:%04d%s", uloff - offset + wave, i * step, i % 3 ? "" : "\n");
    }

    HRGN ur = 0;
    GetClipRgn(dc, ur);
    paint_dc_busy_push();  // popped after the curly clip is cleared below
    IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);

    HPEN oldpen = SelectObject(dc, CreatePen(PS_SOLID, 0, ul));
    for (int l = 0; l < line_width; l++) {
      if (l)
        for (int i = 0; i <= rep * 3; i++)
          bezier[i].y--;
      PolyBezier(dc, (const POINT *)bezier, 1 + rep * 3);
    }
    oldpen = SelectObject(dc, oldpen);
    DeleteObject(oldpen);

    SelectClipRgn(dc, ur);
    paint_dc_busy_pop();
  }
  else

 /* Underline */
#define selfdrawn() (vt52fraction || boxpower || dectcs || boxcoded)
  if (!ldisp2 && lattr != LATTR_TOP &&
      (force_manual_underline
       ||
       // ensure underline of self-drawn characters
       ((nfont & FONT_UNDERLINE) && selfdrawn())
       ||
       (attr.attr & (ATTR_DOUBLYUND | ATTR_BROKENUND))
       ||
       ((attr.attr & UNDER_MASK) == ATTR_UNDER &&
        (ff->und_mode == UND_LINE || (attr.attr & ATTR_ULCOLOUR)))
      )
     )
  {
    clear_run();

    int penstyle = (attr.attr & ATTR_BROKENUND)
                   ? (attr.attr & ATTR_DOUBLYUND)
                     ? PS_DASH
                     : PS_DOT
                   : PS_SOLID;
    HPEN oldpen = SelectObject(dc, CreatePen(penstyle, 0, ul));
    int gapfrom = 0, gapdone = 0;
    int _line_width = line_width;
    if ((attr.attr & UNDER_MASK) == ATTR_DOUBLYUND) {
      if (line_width < 3)
        _line_width = 3;
      int gap = _line_width / 3;
      gapfrom = (_line_width - gap) / 2;
      gapdone = _line_width - gapfrom;
    }
    for (int l = 0; l < _line_width; l++) {
      if (l >= gapdone || l < gapfrom) {
        MoveToEx(dc, x, y + uloff - l, null);
        LineTo(dc, x + ulen * char_width, y + uloff - l);
      }
    }
    oldpen = SelectObject(dc, oldpen);
    DeleteObject(oldpen);
  }

 /* Overline */
  if (!ldisp2 && lattr != LATTR_BOT && attr.attr & ATTR_OVERL) {
    clear_run();

    HPEN oldpen = SelectObject(dc, CreatePen(PS_SOLID, 0, ul));
    for (int l = 0; l < line_width; l++) {
      MoveToEx(dc, x, y + l, null);
      LineTo(dc, x + ulen * char_width, y + l);
    }
    oldpen = SelectObject(dc, oldpen);
    DeleteObject(oldpen);
  }

  int dxs_[len];

 /* Background for overhang overlay */
  if (ldisp1) {
    if (!underlaid)
      clear_run();
    goto _return;  // skipping coord_transformed2 set and restore
  }

 /* Partial glyph adjustments */
  if (vt52fraction) {
    // adjust position of VT52 fraction numerator
    yt -= line_width;
  }

 /* Coordinate transformation per character */
  long long perf_coord_char_t0 = mintty_perf_ticks();
  int coord_transformed2 = 0;
  XFORM old_xform2;
  RECT box_, box2_;
  if (wscale != 100) {
    coord_transformed2 = SetGraphicsMode(dc, GM_ADVANCED);
    if (coord_transformed2 && GetWorldTransform(dc, &old_xform2)) {
      clear_run();

      float scale = (float)wscale / 100.0;
      XFORM xform = (XFORM){scale, 0.0, 0.0, 1.0, xt * (1.0 - scale), 0.0};
      coord_transformed2 = ModifyWorldTransform(dc, &xform, MWT_LEFTMULTIPLY);
      if (coord_transformed2) {
        paint_dc_busy_push();  // popped at the coord_transformed2 restore
      }
      if (coord_transformed2) {
        for (int i = 0; i < len; i++)
          dxs_[i] = dxs[i];
        box_ = box;
        box2_ = box2;
        // compensate for the scaling
        for (int i = 0; i < len; i++)
          dxs[i] /= scale;
        if (wscale <= 100) {
          box.right /= scale;
          box2.right /= scale;
        }
        else { // evolutionary algorithm; don't ask why or how it works :/
        // extend bounding box by the scaling
          box.right *= scale;
          box2.right *= scale;
        }
      }
    }
  }
  PERF_ADD_TICKS(win_text_coord_char_ticks, mintty_perf_ticks() - perf_coord_char_t0);


 /* Finally, draw the text */

  uint overwropt;
  long long perf_bkmode_t0 = mintty_perf_ticks();
  if (ldisp2 || underlaid) {
    SetBkMode(dc, TRANSPARENT);
    PERF_COUNT(set_bk_mode_calls, 1);
    overwropt = 0;
  }
  else {
    SetBkMode(dc, OPAQUE);
    PERF_COUNT(set_bk_mode_calls, 1);
    overwropt = ETO_OPAQUE;
  }
  PERF_ADD_TICKS(set_bk_mode_ticks, mintty_perf_ticks() - perf_bkmode_t0);
  PERF_ADD_TICKS(win_text_bkmode_ticks, mintty_perf_ticks() - perf_bkmode_t0);
  trace_line(" TextOut:");
  // The combining characters separate rendering trick *alone* 
  // makes some combining characters better (~#553, #295), 
  // others worse (#565); however, together with the 
  // substitute combining characters trick it seems to be the best 
  // workaround for combining characters rendering issues.
  // Yet disabling it for some (heuristically determined) cases:
  if (let_windows_combine)
    combining = false;  // disable separate combining characters display

  if (combining || combining_double)
    *dxs = char_width;  // convince Windows to apply font underlining

  // handle invisible and blinking attributes on image background
  if (fg == bg && default_bg && *cfg.background) {
    PERF_COUNT(win_text_skip_invisible_calls, 1);
    goto skip_drawing;  // restore coord_transformed2, then skip self-drawing
  }

  // skip text output for self-drawn characters
  if (origtext) {
    PERF_COUNT(win_text_skip_origtext_calls, 1);
    goto skip_drawing;
  }


 /* Now, really draw the text */

  long long perf_textout_path_t0 = mintty_perf_ticks();
  text_out_start(dc, text, len, dxs);

  // overstrike loop is for shadow or manual bold mode
  for (int xoff0 = 0; xoff0 < xwidth; xoff0++) {
    PERF_COUNT(win_text_overstrike_iterations, 1);
#ifdef configured_glyph_shift
    // calculate glyph shift from character attribute (0..3)
    int xoff = xoff0 + glyph_shift * cell_width / 16;
#else
    // apply centering glyph shift if triggered by attribute (#1313)
    int xoff = xoff0 + glyph_shift;
#endif

    if ((combining || combining_double) && !has_sea) {
      PERF_COUNT(win_text_textout_combining_calls, 1);
      // Workaround for mangled display of combining characters;
      // Arabic shaping should not be affected as the transformed 
      // presentation forms are not combining characters anymore at this point.
      // Repeat the workaround for bold/wide below.

      if (xoff)
        // restore base character colour in case of distinct combining colours
        SetTextColor(dc, fg);

      // base character
      int ulen = char1ulen(text);
      text_out(dc, xt + xoff, yt, eto_options | overwropt, &box, text, ulen, dxs);

      if (overwropt) {
        SetBkMode(dc, TRANSPARENT);
        overwropt = 0;
      }

      // combining characters
      //textattr[0] = attr0; // need unmodified colour attributes for combinings
      // but that spoils separate blinking, so revert commit 736da154
      textattr[0] = attr;
      // (which in turn spoiled substituted combined characters 
      // while combsubst was still in use)

      for (int i = ulen; i < len; i += ulen) {
        // separate stacking of combining characters 
        // does not work with Uniscribe
        use_uniscribe = false;

        int xx = xt + xoff;
        if (combining_double && combiningdouble(text[i]))
          xx += char_width / 2;
        if (!termattrs_equal_fg(&textattr[i], &textattr[i - 1])) {
          // determine colour to be used for combining characters
          colour fg = apply_attr_colour(textattr[i], ACM_SIMPLE).truefg;
          if (layer)
            fg = ((fg & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
          SetTextColor(dc, fg);
        }
        ulen = char1ulen(&text[i]);
        text_out(dc, xx, yt, eto_options, &box2, &text[i], ulen, &dxs[i]);
      }
    }
    else {
      PERF_COUNT(win_text_textout_plain_calls, 1);
      text_out(dc, xt + xoff, yt, eto_options | overwropt, &box, text, len, dxs);
      if (overwropt) {
        SetBkMode(dc, TRANSPARENT);
        overwropt = 0;
      }
    }

    /*
      With image background (-o Background=...png), if output in the 
      top line begins with reverse or coloured background, 
      a mysterious rendering bug hides the first chunk of output 
      in frequent cases at that position.
      (This was traced down in mintty deeply so the remaining suspicion 
      is it's a bug in Windows.)
      As a workaround, we invalidate the top-left cell right away 
      so it gets printed to the window repeatedly, which effectively 
      makes it visible from the first retry.
     */
    if (!tx && !ty && *cfg.background && !default_bg)
      term_invalidate(0, 0, 0, 0);
  }

  long long perf_textout_end_t0 = mintty_perf_ticks();
  text_out_end();
  PERF_ADD_TICKS(win_text_textout_end_ticks, mintty_perf_ticks() - perf_textout_end_t0);
  PERF_ADD_TICKS(win_text_textout_path_ticks, mintty_perf_ticks() - perf_textout_path_t0);

skip_drawing:;


 /* Reset coordinate transformation */
  long long perf_coord_restore_t0 = mintty_perf_ticks();
  if (coord_transformed2) {
    SetWorldTransform(dc, &old_xform2);
    paint_dc_busy_pop();
    // restore these in case we're in a shadow loop
    for (int i = 0; i < len; i++)
      dxs[i] = dxs_[i];
    box = box_;
    box2 = box2_;
  }
  PERF_ADD_TICKS(win_text_coord_restore_ticks, mintty_perf_ticks() - perf_coord_restore_t0);


 /* Skip self-drawing to handle invisible attribute on image background */
  if (fg == bg && default_bg && *cfg.background) {
    PERF_COUNT(win_text_skip_invisible_calls, 1);
    goto _return;
  }


 /* Self-drawn characters: manual drawing of certain graphics */

  // line_width already set above for DEC Tech adjustments
#define dont_debug_vt100_line_drawing_chars
#ifdef debug_vt100_line_drawing_chars
  fg = 0x00FF0000;
#endif

#if __GNUC__ >= 5
#define DRAW_HORIZ 0b1010
#define DRAW_LEFT  0b1000
#define DRAW_RIGHT 0b0010
#define DRAW_VERT  0b0101
#define DRAW_UP    0b0001
#define DRAW_DOWN  0b0100
#else // < 4.3
#define DRAW_HORIZ 0xA
#define DRAW_LEFT  0x8
#define DRAW_RIGHT 0x2
#define DRAW_VERT  0x5
#define DRAW_UP    0x1
#define DRAW_DOWN  0x4
#endif

  void setclipr(int x, int y, int n)
  {
    long long perf_clip_t0 = mintty_perf_ticks();
    int clip_height = cell_height 
                      * (lattr >= LATTR_TOP && ty < term_allrows - 1 ? 2 : 1);
    HRGN clipr = selfdraw_get_clip_rgn(x, y, x + n * char_width, y + clip_height);
    PERF_COUNT(win_text_clip_set_calls, 1);
    // push unconditionally, symmetric with clearclipr's pop; if region
    // creation failed, fills merely fall back to GDI while "busy"
    paint_dc_busy_push();
    if (clipr) {
      SelectClipRgn(dc, clipr);
      PERF_COUNT(win_text_gdi_select_clip_calls, 1);
    }
    PERF_ADD_TICKS(win_text_clip_ticks, mintty_perf_ticks() - perf_clip_t0);
  }
  void clearclipr()
  {
    long long perf_clip_t0 = mintty_perf_ticks();
    SelectClipRgn(dc, 0);
    paint_dc_busy_pop();
    PERF_COUNT(win_text_gdi_select_clip_calls, 1);
    PERF_COUNT(win_text_clip_clear_calls, 1);
    PERF_ADD_TICKS(win_text_clip_ticks, mintty_perf_ticks() - perf_clip_t0);
  }

  long long perf_selfdraw_t0 = mintty_perf_ticks();
  if (vt52fraction) {  // draw VT52 fraction numerators
    long long perf_selfdraw_part_t0 = mintty_perf_ticks();
    setclipr(x, y, ulen);

    selfdraw_pen_ref vpen = selfdraw_get_pen(false, PS_SOLID, line_width, fg);
    HPEN oldpen = SelectObject(dc, vpen.pen);
    PERF_COUNT(win_text_gdi_select_object_calls, 1);

    int xi = x;
    for (int i = 0; i < len; i++) {
      int yt = y + (cell_height - line_width) * 10 / 16;
      int yb = y + (cell_height - line_width) * 8 / 16;
      int xl = xi + line_width - 1;
      int xr = xl + char_width - 1;
      MoveToEx(dc, xl, yt, null);
      LineTo(dc, xr, yb);
      PERF_COUNT(win_text_selfdraw_line_ops, 1);

      xi += char_width;
    }

    SelectObject(dc, oldpen);
    PERF_COUNT(win_text_gdi_select_object_calls, 1);
    selfdraw_release_pen(vpen);

    clearclipr();
    PERF_ADD_TICKS(win_text_selfdraw_vt52_ticks, mintty_perf_ticks() - perf_selfdraw_part_t0);
  }
  else if (boxpower || dectcs) {  // drawn graphics
    long long perf_selfdraw_part_t0 = mintty_perf_ticks();
    // Box Drawing (U+2500-U+257F)
    // ─━│┃┄┅┆┇┈┉┊┋┌┍┎┏┐┑┒┓└┕┖┗┘┙┚┛├┝┞┟┠┡┢┣┤┥┦┧┨┩┪┫┬┭┮┯┰┱┲┳┴┵┶┷┸┹┺┻┼┽┾┿
    // ╀╁╂╃╄╅╆╇╈╉╊╋╌╍╎╏═║╒╓╔╕╖╗╘╙╚╛╜╝╞╟╠╡╢╣╤╥╦╧╨╩╪╫╬╭╮╯╰╱╲╳╴╵╶╷╸╹╺╻╼╽╾╿
    // Block Elements (U+2580-U+259F)
    // ▀▁▂▃▄▅▆▇█▉▊▋▌▍▎▏▐░▒▓▔▕▖▗▘▙▚▛▜▝▞▟
    // Private Use geometric Powerline symbols (U+E0B0-U+E0BF, not 5, 7)
    // 
    //      - -
    int char_height = cell_height;
    if (lattr >= LATTR_TOP)
      char_height *= 2;
    int y0 = y;
    int yclip = y0;
    if (lattr == LATTR_BOT)
      y0 -= cell_height;
    int xi = x;
    //printf("@%d/%d char %d×%d cell %d×%d\n", x, y, char_width, char_height, cell_width, cell_height);

    /*
       Mix fg at mix/8 with bg.
     */
    colour colmix(char mix)
    {
      uint r = (red(fg) * mix + red(bg) * (8 - mix)) / 8;
      uint g = (green(fg) * mix + green(bg) * (8 - mix)) / 8;
      uint b = (blue(fg) * mix + blue(bg) * (8 - mix)) / 8;
      colour res = RGB(r, g, b);
      if (layer)
        res = ((res & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
      return res;
    }
    void linedraw(char l, char t, char r, char b, colour c)
    {
      PERF_COUNT(win_text_selfdraw_linedraw_calls, 1);
      selfdraw_pen_ref lpen = selfdraw_get_pen(false, PS_SOLID, 0, c);
      HPEN oldpen = SelectObject(dc, lpen.pen);
      PERF_COUNT(win_text_gdi_select_object_calls, 1);
      //printf("line %d %d %d %d\n", xi + l, y0 + t, xi + r, y0 + b);
      MoveToEx(dc, xi + l, y0 + t, null);
      LineTo(dc, xi + r, y0 + b);
      PERF_COUNT(win_text_selfdraw_line_ops, 1);
      SelectObject(dc, oldpen);
      PERF_COUNT(win_text_gdi_select_object_calls, 1);
      selfdraw_release_pen(lpen);
    }
    void lines(char x1, char y1, char x2, char y2, char x3, char y3)
    {
      int _x1 = char_width * x1 / 8;
      int _y1 = char_height * y1 / 8;
      int _x2 = char_width * x2 / 8;
      int _y2 = char_height * y2 / 8;
      int _x3 = char_width * x3 / 8;
      int _y3 = char_height * y3 / 8;
      if (x2) {
        _x1--; _x2--; _x3--;
      }

      int w = y3 >= 0 ? line_width : 0;
      selfdraw_pen_ref lpen = selfdraw_get_pen(false, PS_SOLID, w, fg);
      HPEN oldpen = SelectObject(dc, lpen.pen);
      PERF_COUNT(win_text_gdi_select_object_calls, 1);
      MoveToEx(dc, xi + _x1, y0 + _y1, null);
      LineTo(dc, xi + _x2, y0 + _y2);
      PERF_COUNT(win_text_selfdraw_line_ops, 1);
      if (y3 >= 0) {
        MoveToEx(dc, xi + _x3, y0 + _y3 - 1, null);
        LineTo(dc, xi + _x2, y0 + _y2 - 1);
        PERF_COUNT(win_text_selfdraw_line_ops, 1);
      }
      SelectObject(dc, oldpen);
      PERF_COUNT(win_text_gdi_select_object_calls, 1);
      selfdraw_release_pen(lpen);
    }
    void trio(char x1, char y1, char x2, char y2, char x3, char y3, bool chord)
    {
      bool lefthalf = x1;
      if (chord && lefthalf) {
        // Powerline left half circle U+E0B4: fix gap to next cell
        x1++; x2++; x3++;
      }
      int _x1 = char_width * x1 / 8;
      int _y1 = char_height * y1 / 8;
      int _x2 = char_width * x2 / 8;
      int _y2 = char_height * y2 / 8;
      int _x3 = char_width * x3 / 8;
      int _y3 = char_height * y3 / 8;

      selfdraw_pen_ref tpen = selfdraw_get_pen(false, PS_SOLID, 0, fg);
      selfdraw_brush_ref tbrush = selfdraw_get_brush(fg);
      HPEN oldpen = SelectObject(dc, tpen.pen);
      HBRUSH oldbrush = SelectObject(dc, tbrush.brush);
      PERF_COUNT(win_text_gdi_select_object_calls, 2);
      if (chord) {
        if (lefthalf) {
          // Powerline left half circle U+E0B6: trichord(8, 0, 0, 4, 8, 8);
          Chord(dc, xi      , y0 + _y1, xi + 2 * _x1, y0 + _y3,
                    xi + _x1, y0 + _y1, xi + _x3    , y0 + _y3);
          PERF_COUNT(win_text_selfdraw_chord_ops, 1);
        }
        else {
          // Powerline right half circle U+E0B4: trichord(0, 0, 8, 4, 0, 8);
          Chord(dc, xi - _x2, y0 + _y1, xi + _x2, y0 + _y3,
                    xi + _x3, y0 + _y3, xi + _x1, y0 + _y1);
          PERF_COUNT(win_text_selfdraw_chord_ops, 1);
        }
      }
      else {
        Polygon(dc, (POINT[]){{xi + _x1, y0 + _y1},
                              {xi + _x2, y0 + _y2},
                              {xi + _x3, y0 + _y3}}, 3);
        PERF_COUNT(win_text_selfdraw_polygon_ops, 1);
      }
      SelectObject(dc, oldbrush);
      SelectObject(dc, oldpen);
      PERF_COUNT(win_text_gdi_select_object_calls, 2);
      selfdraw_release_brush(tbrush);
      selfdraw_release_pen(tpen);
    }
    void triangle(char x1, char y1, char x2, char y2, char x3, char y3)
    {
      trio(x1, y1, x2, y2, x3, y3, false);
    }
    void trichord(char x1, char y1, char x2, char y2, char x3, char y3)
    {
      trio(x1, y1, x2, y2, x3, y3, true);
    }
    void rectdraw(char l, char t, char r, char b, char sol, colour c)
    {
      // solid 0b1111 top right bottom left
      int cl = char_width * l;
      int ct = char_height * t;
      int cr = char_width * r;
      int cb = char_height * b;
      int dl = cl % 8;
      int dt = ct % 8;
      int dr = cr % 8;
      int db = cb % 8;
      cl /= 8;
      ct /= 8;
      cr /= 8;
      cb /= 8;
      //printf("25XX <%d%%%d %d%%%d %d%%%d %d%%%d\n", cl, dl, ct, dt, cr, dr, cb, db);
      int cl_ = cl;
      int ct_ = ct;
      int cr_ = cr;
      int cb_ = cb;
      if (dl) {
        if (sol & 0x1)
          dl = 0;
        else
          cl_++;
      }
      if (dt) {
        if (sol & 0x8)
          dt = 0;
        else
          ct_++;
      }
      if (dr) {
        if (sol & 0x4) {
          dr = 0;
          cr_++;
        }
      }
      if (db) {
        if (sol & 0x2) {
          db = 0;
          cb_++;
        }
      }
      //printf("25XX >%d%%%d %d%%%d %d%%%d %d%%%d\n", cl, dl, ct, dt, cr, dr, cb, db);
      //printf("Rect %d %d %d %d\n", xi + cl_, y0 + ct_, xi + cr_, y0 + cb_);
      perf_selfdraw_fillrect(dc, &(RECT){xi + cl_, y0 + ct_, xi + cr_, y0 + cb_}, c);
      PERF_COUNT(win_text_selfdraw_rect_ops, 1);
      if (dl)
        linedraw(cl, ct, cl, cb, colmix(8 - dl));
      if (dt)
        linedraw(cl, ct, cr, ct, colmix(8 - dt));
      if (dr)
        linedraw(cr, ct, cr, cb, colmix(dr));
      if (db)
        linedraw(cl, cb, cr, cb, colmix(db));
    }
    inline void rect(char l, char t, char r, char b)
    {
      rectdraw(l, t, r, b, 0, fg);
    }
    inline void rectsolcol(char l, char t, char r, char b, char mix)
    {
      rectdraw(l, t, r, b, 0xF, colmix(mix));
    }
    inline void rectsolid(char l, char t, char r, char b, char sol)
    {
      rectdraw(l, t, r, b, sol, fg);
    }

    // prepare Box Drawing resources
    int penwidth = line_width;
    int heavypenwidth = penwidth + 2;
    // adjust heavy parts in mixed light/heavy boxes:
    int heavydelta = min(line_width, 2);

    // create common Box Drawing resources
    long long perf_selfdraw_resource_t0 = mintty_perf_ticks();
    DWORD style = PS_GEOMETRIC | PS_SOLID;
    selfdraw_pen_ref roundpen_ref = selfdraw_get_pen(true, style, penwidth, fg);
    HPEN roundpen = roundpen_ref.pen;
    if (boxpower)
      style |= PS_ENDCAP_SQUARE;  // skipped for DEC Technical sum segments
    selfdraw_pen_ref pen_ref = selfdraw_get_pen(true, style, penwidth, fg);
    selfdraw_pen_ref heavypen_ref = selfdraw_get_pen(true, style, heavypenwidth, fg);
    HPEN pen = pen_ref.pen;
    HPEN heavypen = heavypen_ref.pen;
    /* Defer selecting a pen until a path actually needs LineTo/AngleArc.
       Common box-drawing characters are now drawn as fills, so preselecting
       and restoring a pen on every tiny self-drawn run is pure GDI state
       churn for the hot TUI path. */
    HPEN oldpen = 0;
    HPEN curpen = 0;
    bool pen_selected = false;
    PERF_ADD_TICKS(win_text_selfdraw_resource_ticks, mintty_perf_ticks() - perf_selfdraw_resource_t0);

    // set pen on demand
    void setpen(HPEN newpen)
    {
      if (newpen != curpen) {
        if (pen_selected)
          SelectObject(dc, newpen);
        else {
          oldpen = SelectObject(dc, newpen);
          pen_selected = true;
        }
        PERF_COUNT(win_text_gdi_select_object_calls, 1);
        curpen = newpen;
      }
    }

#define dl 0x50
#define dh 0x51

    void boxlines(bool heavy, char x1, char y1, char x2, char y2, char x3, char y3)
    {
      int boxscale(int ref, char val)
      {
        if (val >= dl) {
          if (val > dl)
            return ref / 2 + line_width;
          else
            return ref / 2 - line_width;
        }
        else if (y3 < -3) {
          // finer-tuned values for dashed lines
          return ref * val / 72;
        }
        else {
          return ref * val / 24;
        }
      }
      int _x1 = boxscale(char_width, x1);
      int _y1 = boxscale(char_height, y1);
      int _x2 = boxscale(char_width, x2);
      int _y2 = boxscale(char_height, y2);
      int _x3 = boxscale(char_width, x3);
      int _y3 = boxscale(char_height, y3);

      void boxline(int x1, int y1, int x2, int y2)
      {
        //printf("boxline %d/%d..%d/%d w %d\n", x1, y1, x2, y2, line_width);
        // Use rectangles for dashed and axis-aligned strokes. This avoids
        // pen selection and LineTo overhead for the common TUI box characters
        // while keeping diagonals and arcs on the existing pen path.
        if (y3 < -2 || x1 == x2 || y1 == y2) {
          // apply pen width
          int w = penwidth;
          if (heavy)
            w = heavypenwidth;

          // normalise
          if (x1 > x2) {
            x1 ^= x2; x2 ^= x1; x1 ^= x2;
          }
          if (y1 > y2) {
            y1 ^= y2; y2 ^= y1; y1 ^= y2;
          }
          // for vertical line, apply horizontal pen width
          if (x1 == x2) {
            x1 -= w / 2;
            x2 += w - w / 2;
          }
          // for horizontal line, apply vertical pen width
          if (y1 == y2) {
            y1 -= w / 2;
            y2 += w - w / 2;
          }
          //printf("fillrect %d/%d..%d/%d\n", x1, y1, x2, y2);
          perf_selfdraw_fillrect(dc, &(RECT){xi + x1, y0 + y1, xi + x2, y0 + y2}, fg);
          PERF_COUNT(win_text_selfdraw_rect_ops, 1);
        }
        else {
          // for box border lines, we could use FillRect above 
          // in order to get sharp edges;
          // for use of LineTo, we use the PS_ENDCAP_SQUARE pen
          y1 += y0;
          y2 += y0;
          x1 += xi;
          x2 += xi;
          if (heavy)
            setpen(heavypen);
          else if (y3 == -2) {  // diagonals ╲ ╱ ╳
            // without adjustment, the diagonals appear clipped from right,
            // widh adjustment both sides, they appear clipped from left,
            // with adjustment by penwidth / 2, alignment appears bad;
            // penwidth / 3 on the right/bottom side is a compromise
            y2 -= max(penwidth / 3, 1);
            x2 -= max(penwidth / 3, 1);
            // also the square pen appears wrong with the diagonals
            setpen(roundpen);
          }
          else
            setpen(pen);

          // draw the line back again to compensate for the missing endpoint
          //Polyline(dc, (POINT[]){{x1, y1}, {x2, y2}, {x1, y1}}, 3);
          MoveToEx(dc, x1, y1, null);
          LineTo(dc, x2, y2);
          PERF_COUNT(win_text_selfdraw_line_ops, 1);
          // draw the line back again to compensate for the missing endpoint
          if (y3 > -3) {  // skip for dashed line segments
            LineTo(dc, x1, y1);
            PERF_COUNT(win_text_selfdraw_line_ops, 1);
          }
        }
      }

      boxline(_x1, _y1, _x2, _y2);
      if (y3 >= 0)
        boxline(_x2, _y2, _x3, _y3);
    }
    void boxcurve(char q)
    {
      int x1, y1, x2, y2, xc, yc, a;
      //int r = 5;
      int r = char_width / 2 + 1;
      // adjust endpoint by 1 to compensate for the missing endpoint
      switch (q) {
        when 1:  // upper right quarter ╰ lower left border arc
          x1 = char_width / 2;
          y1 = 0;
          x2 = char_width + 1;
          y2 = char_height / 2;
          xc = char_width / 2 + r;
          yc = char_height / 2 - r;
          a = 180;
        when 2:  // lower right quarter ╭ upper left border arc
          x1 = char_width;
          y1 = char_height / 2;
          x2 = char_width / 2;
          y2 = char_height + 1;
          xc = char_width / 2 + r;
          yc = char_height / 2 + r;
          a = 90;
        when 3:  // lower left quarter ╮ upper right border arc
          x1 = char_width / 2;
          y1 = char_height;
          x2 = -1;
          y2 = char_height / 2;
          xc = char_width / 2 - r;
          yc = char_height / 2 + r;
          a = 0;
        when 4:  // upper left quarter ╯ lower right border arc
          x1 = 0;
          y1 = char_height / 2;
          x2 = char_width / 2;
          y2 = -1;
          xc = char_width / 2 - r;
          yc = char_height / 2 - r;
          a = 270;
      }
      setpen(pen);
      MoveToEx(dc, xi + x1, y0 + y1, null);
      AngleArc(dc, xi + xc, y0 + yc, r, a, 90);
      PERF_COUNT(win_text_selfdraw_anglearc_ops, 1);
      LineTo(dc, xi + x2, y0 + y2);
      PERF_COUNT(win_text_selfdraw_line_ops, 1);
    }

    void hline_fill_run(int ymid, int width, int cells)
    {
      int top = ymid - width / 2;
      int bottom = ymid + width - width / 2;
      perf_selfdraw_fillrect(dc, &(RECT){xi, y0 + top, xi + cells * char_width, y0 + bottom}, fg);
      PERF_COUNT(win_text_selfdraw_rect_ops, 1);
    }
    void boxhline_run(bool heavy, int cells)
    {
      hline_fill_run(char_height / 2, heavy ? heavypenwidth : penwidth, cells);
    }
    void boxdhline_run(int cells)
    {
      hline_fill_run(char_height / 2 - line_width, penwidth, cells);
      hline_fill_run(char_height / 2 + line_width, penwidth, cells);
    }

    bool boxpower_char_clip_safe(wchar ch)
    {
      if (ch == ' ')
        return true;
      if (ch >= 0x2580 && ch <= 0x259F)
        return true;
      if (ch >= 0x2500 && ch <= 0x257F)
        return !(ch >= 0x256D && ch <= 0x2573);
      return false;
    }
    bool need_clip = true;
    if (boxpower) {
      need_clip = false;
      for (int i = 0; i < len && !need_clip; i++) {
        need_clip = !boxpower_char_clip_safe(origtext[i]);
      }
    }
    if (need_clip)
      setclipr(xi, yclip, len);
    for (int i = 0; i < len; i++) {
      //setclipr(xi, yclip, 1);

      if (boxpower && i + 1 < len &&
          (origtext[i] == 0x2500 || origtext[i] == 0x2501 || origtext[i] == 0x2550)) {
        int run = 1;
        while (i + run < len && origtext[i + run] == origtext[i])
          run++;
        if (run > 1) {
          if (origtext[i] == 0x2550)
            boxdhline_run(run);
          else
            boxhline_run(origtext[i] == 0x2501, run);
          xi += run * char_width;
          i += run - 1;
          continue;
        }
      }

      switch (origtext[i]) {
        // Box Drawing (U+2500-U+257F)
        // ─━│┃┄┅┆┇┈┉┊┋┌┍┎┏┐┑┒┓└┕┖┗┘┙┚┛├┝┞┟┠┡┢┣┤┥┦┧┨┩┪┫┬┭┮┯┰┱┲┳┴┵┶┷┸┹┺┻┼┽┾┿
        // ╀╁╂╃╄╅╆╇╈╉╊╋╌╍╎╏═║╒╓╔╕╖╗╘╙╚╛╜╝╞╟╠╡╢╣╤╥╦╧╨╩╪╫╬╭╮╯╰╱╲╳╴╵╶╷╸╹╺╻╼╽╾╿
// tune position and length of double/triple dash segments
#define sub2 line_width
#define add2 2 * line_width
#define sub3 line_width
#define add3 line_width
#include "boxdrawing.t"

        // DEC Technical Character Set (TCS)
        // !1234567DE
        // ⎷  ╲╱   Δ∇
        when '!': // RADICAL SYMBOL BOTTOM
          boxlines(false, 12, 0, 12, 24, -1, -1);
          boxlines(false, 0, 12, 5, 12, 12, 24);
        when '"':
          boxlines(false, 12, 24, 12, 12, 24, 12);
        when '#':
          boxlines(false, 0, 12, 24, 12, -1, -1);
        when '1': // Top Left Sigma
          boxlines(false, 24, 12, 12, 12, 24, 24);
        when '2': // Bottom Left Sigma
          boxlines(false, 24, 12, 12, 12, 24, 0);
        when '3': // Top Diagonal Sigma
          boxlines(false, 0, 0, 24, 24, -2, -2);
        when '4': // Bottom Diagonal Sigma
          boxlines(false, 0, 24, 24, 0, -2, -2);
#define sigend 18
        when '5': // Top Right Sigma
          boxlines(false, 0, 12, sigend, 12, sigend, 22);
        when '6': // Bottom Right Sigma
          boxlines(false, 0, 12, sigend, 12, sigend, 2);
        when '7': // Middle Sigma
          boxlines(false, 0, 0, 12, 12, 0, 24);
#define nabdel 8
        when 'D': // DELTA
          boxlines(false, 12, 2, 12 - nabdel, 22, 12 + nabdel, 22);
          boxlines(false, 12, 2, 12 + nabdel, 22, -1, -1);
        when 'E': // NABLA
          boxlines(false, 12, 22, 12 - nabdel, 2, 12 + nabdel, 2);
          boxlines(false, 12, 22, 12 + nabdel, 2, -1, -1);

        // Private Use geometric Powerline symbols (U+E0B0-U+E0BF, not 5, 7)
        // 
        //      - -
        when 0xE0B0:
          triangle(0, 0, 8, 4, 0, 8);
        when 0xE0B1:
          lines(0, 0, 8, 4, 0, 8);
        when 0xE0B2:
          triangle(8, 0, 0, 4, 8, 8);
        when 0xE0B3:
          lines(8, 0, 0, 4, 8, 8);
        when 0xE0B4:
          trichord(0, 0, 8, 4, 0, 8);
        when 0xE0B6:
          trichord(8, 0, 0, 4, 8, 8);
        when 0xE0B8:
          triangle(0, 0, 0, 8, 8, 8);
        when 0xE0B9:
          lines(0, 0, 8, 8, -1, -1);
        when 0xE0BA:
          triangle(8, 0, 8, 8, 0, 8);
        when 0xE0BB:
          lines(8, 0, 0, 8, -1, -1);
        when 0xE0BC:
          triangle(0, 0, 8, 0, 0, 8);
        when 0xE0BD:
          lines(8, 0, 0, 8, -1, -1);
        when 0xE0BE:
          triangle(0, 0, 8, 0, 8, 8);
        when 0xE0BF:
          lines(0, 0, 8, 8, -1, -1);

        // Block Elements (U+2580-U+259F)
        // ▀▁▂▃▄▅▆▇█▉▊▋▌▍▎▏▐░▒▓▔▕▖▗▘▙▚▛▜▝▞▟
        when 0x2580: rect(0, 0, 8, 4); // UPPER HALF BLOCK
        when 0x2581 ... 0x2588: rect(0, 0x2588 - origtext[i], 8, 8); // LOWER EIGHTHS
        when 0x2589 ... 0x258F: rect(0, 0, 0x2590 - origtext[i], 8); // LEFT EIGHTHS
        when 0x2590: rect(4, 0, 8, 8); // RIGHT HALF BLOCK
        when 0x2591: rectsolcol(0, 0, 8, 8, 2); // ░ LIGHT SHADE
        when 0x2592: rectsolcol(0, 0, 8, 8, 3); // ▒ MEDIUM SHADE
        when 0x2593: rectsolcol(0, 0, 8, 8, 5); // ▓ DARK SHADE
        when 0x2594: rect(0, 0, 8, 1); // UPPER ONE EIGHTH BLOCK
        when 0x2595: rect(7, 0, 8, 8); // RIGHT ONE EIGHTH BLOCK
        when 0x2596: rect(0, 4, 4, 8);
        when 0x2597: rect(4, 4, 8, 8);
        when 0x2598: rect(0, 0, 4, 4);
        // solid 0b1111 top right bottom left
        when 0x2599: rectsolid(0, 4, 4, 8, 0xF);
                   rectsolid(0, 0, 4, 4, 0xB);
                   rectsolid(4, 4, 8, 8, 0x7);
        when 0x259A: rect(0, 0, 4, 4); rect(4, 4, 8, 8);
        when 0x259B: rectsolid(0, 0, 4, 4, 0xF);
                   rectsolid(4, 0, 8, 4, 0xD);
                   rectsolid(0, 4, 4, 8, 0xB);
        when 0x259C: rectsolid(4, 0, 8, 4, 0xF);
                   rectsolid(0, 0, 4, 4, 0xD);
                   rectsolid(4, 4, 8, 8, 0xE);
        when 0x259D: rect(4, 0, 8, 4);
        when 0x259E: rect(4, 0, 8, 4); rect(0, 4, 4, 8);
        when 0x259F: rectsolid(4, 4, 8, 8, 0xF);
                   rectsolid(4, 0, 8, 4, 0xE);
                   rectsolid(0, 4, 4, 8, 0x7);
      }

      //clearclipr();

      xi += char_width;
    }
    if (need_clip)
      clearclipr();

    // remove Box Drawing resources
    long long perf_selfdraw_teardown_t0 = mintty_perf_ticks();
    if (pen_selected) {
      SelectObject(dc, oldpen);
      PERF_COUNT(win_text_gdi_select_object_calls, 1);
    }
    selfdraw_release_pen(pen_ref);
    selfdraw_release_pen(roundpen_ref);
    selfdraw_release_pen(heavypen_ref);
    PERF_ADD_TICKS(win_text_selfdraw_teardown_ticks, mintty_perf_ticks() - perf_selfdraw_teardown_t0);
    PERF_ADD_TICKS(win_text_selfdraw_boxpower_ticks, mintty_perf_ticks() - perf_selfdraw_part_t0);
  }
  else if (boxcoded && origtext) {  // VT100/VT52 box drawing and scanlines
    long long perf_selfdraw_part_t0 = mintty_perf_ticks();
    selfdraw_pen_ref bpen = selfdraw_get_pen(false, PS_SOLID, 0, fg);
    HPEN oldpen = SelectObject(dc, bpen.pen);
    PERF_COUNT(win_text_gdi_select_object_calls, 1);

    int xi = x;
    for (int i = 0; i < len; i++) {
      setclipr(xi, y, 1);

      int graph = origtext[i];
      bool graph_vt52 = false;
      if (graph > 0x500) {
        graph_vt52 = true;
        graph &= 0xF;
        graph <<= 4;  // indicate VT52 scanlines where expected
      }
      else {
        graph -= 0x100;
      }
      if (graph >> 4) {  // VT100/VT52 horizontal "scanlines"
        int parts = graph_vt52 ? 8 : 5;
        int yoff = (cell_height - line_width) * (graph >> 4) / parts;
        if (lattr >= LATTR_TOP)
          yoff *= 2;
        if (lattr == LATTR_BOT)
          yoff -= cell_height;
        for (int l = 0; l < line_width; l++) {
          MoveToEx(dc, x, y + yoff + l, null);
          LineTo(dc, x + len * char_width, y + yoff + l);
          PERF_COUNT(win_text_selfdraw_line_ops, 1);
        }
      }
      else {  // VT100 box drawing characters ┘┐┌└┼ ─ ├┤┴┬│
        int y0 = (lattr == LATTR_BOT) ? y - cell_height : y;
        int yoff = (cell_height - line_width) * 3 / 5;
        if (lattr >= LATTR_TOP)
          yoff *= 2;
        int xoff = (char_width - line_width) / 2;

        if (graph & DRAW_HORIZ) {
          int xl, xr;
          if (graph & DRAW_LEFT)
            xl = x + i * char_width;
          else
            xl = x + i * char_width + xoff;
          if (graph & DRAW_RIGHT)
            xr = x + (i + 1) * char_width;
          else
            xr = x + i * char_width + xoff + line_width;
          for (int l = 0; l < line_width; l++) {
            MoveToEx(dc, xl, y0 + yoff + l, null);
            LineTo(dc, xr, y0 + yoff + l);
            PERF_COUNT(win_text_selfdraw_line_ops, 1);
          }
        }
        if (graph & DRAW_VERT) {
          int xi = x + i * char_width + xoff;
          int yt, yb;
          if (graph & DRAW_UP)
            yt = y0;
          else
            yt = y0 + yoff;
          if (graph & DRAW_DOWN)
            yb = y0 + (lattr >= LATTR_TOP ? 2 : 1) * cell_height;
          else
            yb = y0 + yoff + line_width;
          for (int l = 0; l < line_width; l++) {
            MoveToEx(dc, xi + l, yt, null);
            LineTo(dc, xi + l, yb);
            PERF_COUNT(win_text_selfdraw_line_ops, 1);
          }
        }
      }

      clearclipr();

      xi += char_width;
    }

    SelectObject(dc, oldpen);
    PERF_COUNT(win_text_gdi_select_object_calls, 1);
    selfdraw_release_pen(bpen);
    PERF_ADD_TICKS(win_text_selfdraw_boxcoded_ticks, mintty_perf_ticks() - perf_selfdraw_part_t0);
  }
  if (vt52fraction || boxpower || dectcs || boxcoded)
    PERF_ADD_TICKS(win_text_selfdraw_ticks, mintty_perf_ticks() - perf_selfdraw_t0);

 /* Strikeout */
  if ((attr.attr & ATTR_STRIKEOUT)
      //&& !ldisp1
      && (cfg.underl_manual || cfg.underl_colour != (colour)-1
          || (attr.attr & ATTR_ULCOLOUR)
          || origtext  // apply strikeout to self-drawn characters
         )
     )
  {
    int soff = (ff->descent + (ff->row_spacing / 2)) * 2 / 3;
    HPEN oldpen = SelectObject(dc, CreatePen(PS_SOLID, 0, ul));
    for (int l = 0; l < line_width; l++) {
      MoveToEx(dc, x, y + soff + l, null);
      LineTo(dc, x + ulen * char_width, y + soff + l);
    }
    oldpen = SelectObject(dc, oldpen);
    DeleteObject(oldpen);
  }

  _return:

  show_curchar_info('w');

  if (has_cursor && phase < 2) {
    int cursor_size(int cell_size)
    {
      switch (term.cursor_size ?: cfg.cursor_size) {
        when 1: return -2;                // invisible
        when 2: return line_width - 1;    // underscore
        when 3: return cell_size / 3 - 1; // ⅓
        when 4: return cell_size / 2;     // ½
        when 5: return cell_size * 2 / 3; // ⅔
        when 6: return cell_size - 2;     // full block
        otherwise: return 0;              // default
      }
    }

    colour _cc = cursor_colour;
    if (layer)
      _cc = ((_cc & 0xFEFEFEFE) >> 1) + ((win_get_colour(BG_COLOUR_I) & 0xFEFEFEFE) >> 1);
#if defined(debug_cursor) && debug_cursor > 1
    printf("painting cursor_type '%c' cursor_on %d\n", "?b_l"[term_cursor_type()+1], term.cursor_on);
#endif
    HPEN oldpen = SelectObject(dc, CreatePen(PS_SOLID, 0, _cc));
    PERF_COUNT(win_text_gdi_create_pen_calls, 1);
    PERF_COUNT(win_text_gdi_select_object_calls, 1);
    switch (term_cursor_type()) {
      when CUR_BLOCK:  // solid block cursor
        if (attr.attr & TATTR_PASCURS) {
          HBRUSH oldbrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
          Rectangle(dc, x, y, x + char_width, y + cell_height);
          PERF_COUNT(win_text_selfdraw_rect_ops, 1);
          SelectObject(dc, oldbrush);
          PERF_COUNT(win_text_gdi_select_object_calls, 2);
        }
      when CUR_BOX: {  // hollow box cursor
        HBRUSH oldbrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x, y, x + char_width, y + cell_height);
        PERF_COUNT(win_text_selfdraw_rect_ops, 1);
        SelectObject(dc, oldbrush);
        PERF_COUNT(win_text_gdi_select_object_calls, 2);
      }
      when CUR_LINE: {  // vertical line cursor
        int caret_width = cursor_size(cell_width);
        if (caret_width <= 0) {
          caret_width = (3 + (lattr >= LATTR_WIDE ? 2 : 0)) * cell_width / 40;
          SystemParametersInfo(SPI_GETCARETWIDTH, 0, &caret_width, 0);
          caret_width *= cell_width / 8;
          int min_caret_width = dpi / 72;
          // limit cursor width (max previously by line_width, #1101)
          if (caret_width < min_caret_width)
            caret_width = min_caret_width;
          else if (caret_width > cell_width)
            caret_width = cell_width;
        }
        int xx = x;
        if (attr.attr & TATTR_RIGHTCURS)
          xx += char_width - caret_width;
        if (attr.attr & TATTR_ACTCURS) {
#ifdef cursor_painted_with_rectangle
          // this would add an additional line, vanishing again but 
          // leaving a pixel artefact, under some utterly weird interference 
          // with output of certain characters (mintty/wsltty#255)
          HBRUSH oldbrush = SelectObject(dc, CreateSolidBrush(_cc));
          Rectangle(dc, xx, y, xx + caret_width, y + cell_height);
          DeleteObject(SelectObject(dc, oldbrush));
#else
#ifdef simple_inverted_cursor_approach
          // this does not give us sufficient colour control
          InvertRect(dc, &(RECT){xx, y, xx + caret_width, y + cell_height});
#else
          perf_selfdraw_fillrect(dc, &(RECT){xx, y, xx + caret_width, y + cell_height}, _cc);
          PERF_COUNT(win_text_selfdraw_rect_ops, 1);
#endif
#endif
        }
        else if (attr.attr & TATTR_PASCURS) {
          for (int dy = 0; dy < cell_height; dy += 2) {
            Polyline(
              dc, (POINT[]){{xx, y + dy}, {xx + caret_width, y + dy}}, 2);
            PERF_COUNT(win_text_selfdraw_line_ops, 1);
          }
        }
      }
      when CUR_UNDERSCORE: {  // horizontal line cursor
        int yy = yt + min(ff->descent, cell_height - 2);
        yy += ff->row_spacing * 3 / 8;
        if (lattr >= LATTR_TOP) {
          yy += ff->row_spacing / 2;
          if (lattr == LATTR_BOT)
            yy += cell_height;
        }
        if (attr.attr & TATTR_ACTCURS) {
	  /* cursor size CSI ? N c
	     from linux console https://linuxgazette.net/137/anonymous.html
		0   default
		1   invisible
		2   underscore
		3   lower_third
		4   lower_half
		5   two_thirds
		6   full block
          */
          int up = cursor_size(cell_height);
          if (up) {
            int yct = max(yy - up, yt);
            HBRUSH oldbrush = SelectObject(dc, CreateSolidBrush(_cc));
            Rectangle(dc, x, yct, x + char_width, yy + 2);
            PERF_COUNT(win_text_selfdraw_rect_ops, 1);
            DeleteObject(SelectObject(dc, oldbrush));
            PERF_COUNT(win_text_gdi_select_object_calls, 1);
            PERF_COUNT(win_text_gdi_delete_object_calls, 1);
          }
          else {
            Rectangle(dc, x, yy - up, x + char_width, yy + 2);
            PERF_COUNT(win_text_selfdraw_rect_ops, 1);
          }
        }
        else if (attr.attr & TATTR_PASCURS) {
          for (int dx = 0; dx < char_width; dx += 2) {
            SetPixel(dc, x + dx, yy, _cc);
            SetPixel(dc, x + dx, yy + 1, _cc);
            PERF_COUNT(win_text_selfdraw_setpixel_ops, 2);
          }
        }
      }
    }
    DeleteObject(SelectObject(dc, oldpen));
    PERF_COUNT(win_text_gdi_select_object_calls, 1);
    PERF_COUNT(win_text_gdi_delete_object_calls, 1);
  }

  if (layer) {
    layer--;
    yt = yt0;
    xt = xt0;
    y = y0;
    x = x0;
    if (!layer) {
      xt -= line_width;
      x -= line_width;
      fg = fg0;
      ul = ul0;
      SetTextColor(dc, fg);
    }
    yt += line_width;
    y += line_width;
    underlaid = true;
    goto draw;
  }

  if (coord_transformed) {
    SetWorldTransform(dc, &old_xform);
    paint_dc_busy_pop();
  }
  PERF_ADD_TICKS(win_text_ticks, mintty_perf_ticks() - perf_win_text_start);
}


static HFONT
font4(struct fontfam * ff, cattrflags attr)
{
  bool bold = (ff->bold_mode == BOLD_FONT) && (attr & ATTR_BOLD);
  bool italic = attr & ATTR_ITALIC;
  int font4index = (bold ? FONT_BOLD : 0) | (italic ? FONT_ITALIC : 0);
  HFONT f = ff->fonts[font4index];
  if (!f && italic) {
    if (!ff->fonts[FONT_BOLD]) {
      font4index &= ~FONT_BOLD;
      f = ff->fonts[font4index];
    }
    if (!f) {
      another_font(ff, font4index);
      f = ff->fonts[font4index];
    }
  }
  if (!f)
    f = ff->fonts[FONT_NORMAL];
  return f;
}

/* Check availability of characters in the current font.
 * Zeroes each of the characters in the input array that isn't available.
 */
void
win_check_glyphs(wchar *wcs, uint num, cattrflags attr)
{
  int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;

  struct fontfam * ff = &fontfamilies[findex];

  HFONT f = font4(ff, attr);

  HDC dc = GetDC(wnd);
  SelectObject(dc, f);
  ushort glyphs[num];
  GetGlyphIndicesW(dc, wcs, num, glyphs, true);

  // recheck for characters affected by FontChoice
  for (uint i = 0; i < num; i++) {
    uchar cf = scriptfont(wcs[i]);
    cf &= 0xF;  // mask glyph shift / glyph centering flag
#ifdef debug_scriptfonts
    if (wcs[i] && cf)
      printf("scriptfont %04X: %d\n", wcs[i], cf);
#endif
    if (cf && cf <= 10) {
      struct fontfam * ff = &fontfamilies[cf];
      f = font4(ff, attr);
      SelectObject(dc, f);
      GetGlyphIndicesW(dc, &wcs[i], 1, &glyphs[i], true);
    }
  }

  for (uint i = 0; i < num; i++) {
    if (glyphs[i] == 0xFFFF || glyphs[i] == 0x1F)
      wcs[i] = 0;
  }

#ifdef check_font_ranges
#warning this does not tell us whether a glyph is shown
  bool bold = (ff->bold_mode == BOLD_FONT) && (attr & ATTR_BOLD);
  bool italic = attr & ATTR_ITALIC;
  int font4index = (bold ? FONT_BOLD : 0) | (italic ? FONT_ITALIC : 0);
  GLYPHSET * gs = win_font_ranges(dc, ff, font4index);
  if (gs)
    for (uint i = 0; i < num; i++) {
      if (!glyph_in(wcs[i], gs))
        wcs[i] = 0;
    }
#endif

  ReleaseDC(wnd, dc);
}

#if CYGWIN_VERSION_DLL_MAJOR >= 3000
#define use_dwrite
#endif

#ifdef use_dwrite
#define COBJMACROS
#include <dwrite.h>
static const IID MY_IID_IDWriteFactory =
  { 0xb859ee5a, 0xd838, 0x4b5b,
    { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
#endif

/* Check availability of characters in the current font.
 * Zeroes each of the characters in the input array that isn't available.
 * Rather than the GDI function GetGlyphIndices which is not capable 
   of handling non-BMP characters, we use a DirectWrite function for this.
 */
bool
dw_check_glyphs(xchar * xcs, uint num, cattrflags attr)
{
#ifdef use_dwrite

  int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;

  struct fontfam * ff = &fontfamilies[findex];
  HFONT f = font4(ff, attr);

  HDC dc = GetDC(wnd);
  SelectObject(dc, f);

  HRESULT hr;
  bool ok = false;
  UINT16 glyphIndex[num];

  // singular DWrite objects
  static IDWriteFactory * factory = 0;
  static IDWriteGdiInterop * interop = 0;

  // case-by-case DWrite object
  IDWriteFontFace * fontFace = 0;

  if (!factory) {
    hr = DWriteCreateFactory(
           DWRITE_FACTORY_TYPE_SHARED,
           //&IID_IDWriteFactory,  // does not link in cygwin
           &MY_IID_IDWriteFactory,
           (IUnknown**)&factory
    );
    if (FAILED(hr))
      goto cleanup;
  }

  if (!interop) {
    hr = IDWriteFactory_GetGdiInterop(factory, &interop);
    if (FAILED(hr))
      goto cleanup;
  }

  hr = IDWriteGdiInterop_CreateFontFaceFromHdc(interop, dc, &fontFace);
  if (FAILED(hr))
    goto cleanup;

  hr = IDWriteFontFace_GetGlyphIndices(fontFace, xcs, num, glyphIndex);
  if (FAILED(hr))
    goto cleanup;

  if (!(attr & DATTR_STARTRUN))
    // recheck for characters affected by FontChoice
    for (uint i = 0; i < num; i++) {
      uchar cf = scriptfont(xcs[i]);
      cf &= 0xF;  // mask glyph shift / glyph centering flag
#ifdef debug_scriptfonts
      if (xcs[i] && cf)
        printf("scriptfont %04X: %d\n", xcs[i], cf);
#endif
      if (cf && cf <= 10) {
        struct fontfam * ff = &fontfamilies[cf];
        f = font4(ff, attr);
        SelectObject(dc, f);
        IDWriteFontFace * fontFace = 0;
        hr = IDWriteGdiInterop_CreateFontFaceFromHdc(interop, dc, &fontFace);
        if (SUCCEEDED(hr)) {
          hr = IDWriteFontFace_GetGlyphIndices(fontFace, &xcs[i], 1, &glyphIndex[i]);
          IDWriteFontFace_Release(fontFace);
        }
      }
    }

  ok = true;
  // indicate missing glyphs in parameter array
  for (uint i = 0; i < num; i++)
    if (!glyphIndex[i])
      xcs[i] = 0;

cleanup:
  // drop font object
  if (fontFace)
    IDWriteFontFace_Release(fontFace);
  // keep singular objects
#ifdef dont_keep_dwrite_singulars
  if (interop)
    IDWriteGdiInterop_Release(interop);
  if (factory)
    IDWriteFactory_Release(factory);  //factory->lpVtbl->Release(factory);
#endif

  ReleaseDC(wnd, dc);
  return ok;

#else
  (void)xcs, (void)num, (void)attr;
  return false;
#endif
}

/* Check availability of single character in the current font.
 * Use a DirectWrite function for this.
 */
bool
dw_has_glyph(xchar xc, cattrflags attr)
{
#ifdef has_glyph_use_check_glyphs
  // defer to glyph checking function, with large performance impact
  xchar c1 = xc;
  dw_check_glyphs(&c1, 1, attr | DATTR_STARTRUN);
  return c1;
#endif

#ifdef use_dwrite

  int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;
  uchar cf = scriptfont(xc);
  cf &= 0xF;  // mask glyph shift / glyph centering flag
#ifdef debug_scriptfonts
  if (xc && cf)
    printf("scriptfont %04X: %d\n", xc, cf);
#endif
  if (cf && cf <= 10)
    findex = cf;

  struct fontfam * ff = &fontfamilies[findex];
  bool ok = false;

  HRESULT hr;

  // singular DWrite objects
  static IDWriteFactory * factory = 0;
  static IDWriteGdiInterop * interop = 0;

  if (!factory) {
    hr = DWriteCreateFactory(
           DWRITE_FACTORY_TYPE_SHARED,
           //&IID_IDWriteFactory,  // does not link in cygwin
           &MY_IID_IDWriteFactory,
           (IUnknown**)&factory
    );
    if (FAILED(hr))
      goto cleanup;
  }

  if (!interop) {
    hr = IDWriteFactory_GetGdiInterop(factory, &interop);
    if (FAILED(hr))
      goto cleanup;
  }

  // cache font objects used for detection
  // the flag in the fontfamilies struct indicates the need to refresh
  static IDWriteFont * font[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  if (!ff->cached) {
    // drop previous font object
    if (font[findex]) {
      IDWriteFont_Release(font[findex]);
      font[findex] = 0;
    }

    int w = (attr & ATTR_BOLD) ? ff->fw_bold : ff->fw_norm;
    int i = attr & ATTR_ITALIC;
    LOGFONTW lf = {font_height, 0, 0, 0, w, i, false, false,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        get_font_quality(), FIXED_PITCH | FF_DONTCARE,
        W("")};
    wcsncpy(lf.lfFaceName, ff->name, LF_FACESIZE);
    hr = IDWriteGdiInterop_CreateFontFromLOGFONT(interop, &lf, &font[findex]);
    if (FAILED(hr))
      goto cleanup;
    ff->cached = true;
    //printf("CreateFontFromLOGFONT [%d] (%ls)\n", findex, ff->name);
  }
  BOOL ex;
  hr = IDWriteFont_HasCharacter(font[findex], xc, &ex);
  //printf("HasCharacter (ok %d): %d\n", SUCCEEDED(hr), ex);
  if (FAILED(hr))
    ok = true;  // could not detect -> do not trigger fallback
  else
    ok = ex;

cleanup:
  // keep singular objects
#ifdef dont_keep_dwrite_singulars
  if (interop)
    IDWriteGdiInterop_Release(interop);
  if (factory)
    IDWriteFactory_Release(factory);  //factory->lpVtbl->Release(factory);
#endif

  return ok;

#else
  (void)xc, (void)attr;
  return true;  // cannot detect -> do not trigger fallback
#endif
}


wchar
get_errch(wchar *wcs, cattrflags attr)
{
  int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;

  struct fontfam * ff = &fontfamilies[findex];
  if (!ff->errch) {
    static wchar * errchars = 0;
    if (!errchars)
      errchars = wcsdup(wcs);

    win_check_glyphs(errchars, wcslen(wcs) - 1, term.curs.attr.attr);
    for (uint i = 0; i < wcslen(wcs); i++)
      if (errchars[i]) {
        ff->errch = wcs[i];
        break;
      }
  }
  return ff->errch;
}

#define dont_debug_win_char_width 2

#ifdef debug_win_char_width
int
win_char_width(xchar c, cattrflags attr)
{
#define win_char_width xwin_char_width
int win_char_width(xchar, cattrflags);
  ulong t = mtime();
  int w = win_char_width(c, attr);
  if (c >= 0x80)
    printf(" [%ld:%ld] win_char_width(%04X) -> %d\n", t, mtime() - t, c, w);
  return w;
}
#endif

/* This function gets the actual width of a character in the normal font.
   Usage:
   * determine whether to trim an ambiguous wide character 
     (of a CJK ambiguous-wide font such as BatangChe) to normal width 
     if desired.
   * also whether to expand a normal width character if expected wide
   This is the uncached implementation; win_char_width below wraps it 
   with a memoisation cache (see the wcw_* functions further above).
 */
static int
win_char_width_uncached(xchar c, cattrflags attr)
{
  // NOTE: if wintext.c is compiled with optimization (-O1 or higher), 
  // and win_char_width is called for a non-BMP character (>= 0x10000), 
  // (unless inhibited by calls to GetCharABCWidths* below)
  // a mysterious delay occurs, if tracing with timestamps apparently 
  // *before* invocation of win_char_width (unless printf gets delayed...)

  int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
  if (findex > 10)
    findex = 0;
  struct fontfam * ff = &fontfamilies[findex];
#ifdef debug_win_char_width
  if (c > 0xFF)
    printf("win_char_width(%04X) font %d\n", c, findex);
#endif

#define measure_width

#if ! defined(measure_width) && ! defined(debug_win_char_width)
 /* If the font max width is the same as the font average width
  * then this function is a no-op.
  */
  if (!ff->font_dualwidth)
    // this optimization ignores font fallback and should be dropped 
    // if ever a more particular width checking is implemented (#615)
    return 1;
#endif

 /* Speedup, I know of no font where ASCII is the wrong width */
#ifdef debug_win_char_width
  if (c != 'A')
#endif
  if (c >= ' ' && c <= '~')  // don't width-check ASCII
    return 1;

  HFONT f = font4(ff, attr);

  HDC dc = GetDC(wnd);
#ifdef debug_win_char_width
  bool ok0 = !!
#endif
  SelectObject(dc, f);
#ifdef debug_win_char_width
  if (c == 0x2001)
    win_char_width(0x5555, attr);
  if (!ok0)
    printf(" wdth %04X failed (dc %p)\n", c, dc);
  else if (c > '~' || c == 'A') {
    int cw = 0;
    BOOL ok1 = GetCharWidth32W(dc, c, c, &cw);  // "not on TrueType"
    float cwf = 0.0;
    BOOL ok2 = GetCharWidthFloatW(dc, c, c, &cwf);
    ABC abc; memset(&abc, 0, sizeof abc);
    // NOTE: with these 2 calls of GetCharABCWidths*, 
    // the mysterious delay for non-BMP characters does not occur, 
    // again mysteriously
    BOOL ok3 = GetCharABCWidthsW(dc, c, c, &abc);  // only on TrueType
    ABCFLOAT abcf; memset(&abcf, 0, sizeof abcf);
    BOOL ok4 = GetCharABCWidthsFloatW(dc, c, c, &abcf);
    printf(" w %04X [cell %d] - 32 %d %d - flt %d %.3f - abc %d %d %d %d - abcflt %d %4.1f %4.1f %4.1f\n", 
           c, cell_width, 
           ok1, cw, ok2, cwf, 
           ok3, abc.abcA, abc.abcB, abc.abcC, 
           ok4, abcf.abcfA, abcf.abcfB, abcf.abcfC);
  }
#endif

  int ibuf = 0;

  if (c < 0x10000) {
    // use GetCharWidth* for BMP only;
    // used to avoid GetCharWidth* at all from 3.4.4 to 3.5.3
    // but we need it to support @cjkwide auto-widening
    bool ok = GetCharWidth32W(dc, c, c, &ibuf);
#ifdef debug_win_char_width
    printf(" getcharwidth32 %04X %dpx(/cell %dpx)\n", c, ibuf, cell_width);
#endif
    if (!ok) {
      ReleaseDC(wnd, dc);
      return 0;
    }

    // report char as wide if its width is more than 1½ cells;
    // this is unreliable if font fallback is involved (#615)
    ibuf += cell_width / 2 - 1;
    ibuf /= cell_width;
    if (ibuf > 1) {
#ifdef debug_win_char_width
      printf(" enquired %04X %dpx/cell %dpx\n", c, ibuf, cell_width);
#endif
      ReleaseDC(wnd, dc);
      //printf(" win_char_width %04X -> %d\n", c, ibuf);
      return ibuf;
    }
  }

#ifdef measure_width

#define dont_debug_rendering

  int act_char_width(xchar wc)
  {
# ifdef debug_rendering
# include <time.h>
    struct timespec tim;
    clock_gettime(CLOCK_MONOTONIC, &tim);
    ulong now = tim.tv_sec * (long)1000000000 + tim.tv_nsec;
# endif
    HDC wid_dc = CreateCompatibleDC(dc);
    HBITMAP wid_bm = CreateCompatibleBitmap(dc, cell_width * 2, cell_height);
    HBITMAP wid_oldbm = SelectObject(wid_dc, wid_bm);
    SelectObject(wid_dc, ff->fonts[FONT_NORMAL]);
    SetTextAlign(wid_dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    SetTextColor(wid_dc, RGB(255, 255, 255));
    SetBkColor(wid_dc, RGB(0, 0, 0));
    SetBkMode(wid_dc, OPAQUE);
    int dx = 0;
    use_uniscribe = cfg.font_render == FR_UNISCRIBE;
    wchar wc2[2];
    if (wc < 0x10000) {
      *wc2 = wc;
      text_out_start(wid_dc, wc2, 1, &dx);
      text_out(wid_dc, 0, 0, ETO_OPAQUE, null, wc2, 1, &dx);
    }
    else {
      wc2[0] = high_surrogate(wc);
      wc2[1] = low_surrogate(wc);
      text_out_start(wid_dc, wc2, 2, &dx);
      text_out(wid_dc, 0, 0, ETO_OPAQUE, null, wc2, 2, &dx);
    }
    text_out_end();

    int wid = 0;

//#define debug_win_char_width 2

#ifdef use_GetPixel

# if defined(debug_win_char_width) && debug_win_char_width > 1
    for (int y = 0; y < cell_height; y++) {
      printf(" %2d|", y);
      for (int x = 0; x < cell_width * 2; x++) {
        COLORREF c = GetPixel(wid_dc, x, y);
        printf("%c", c != RGB(0, 0, 0) ? '*' : ' ');
      }
      printf("|\n");
    }
# endif
# ifdef heuristic_sparse_width_checking
    for (int x = cell_width * 2 - 1; !wid && x >= cell_width; x -= 2)
      for (int y = 0; y < cell_height / 2; y++) {
        COLORREF c = GetPixel(wid_dc, x, cell_height / 2 + y);
        if (c != RGB(0, 0, 0)) {
          wid = x + 1;
          break;
        }
        c = GetPixel(wid_dc, x, cell_height / 2 - y);
        if (c != RGB(0, 0, 0)) {
          wid = x + 1;
          break;
        }
      }
# else
    for (int x = cell_width * 2 - 1; !wid && x >= 0; x--)
      for (int y = 0; y < cell_height; y++) {
        COLORREF c = GetPixel(wid_dc, x, y);
        if (c != RGB(0, 0, 0)) {
          wid = x + 1;
          break;
        }
      }
# endif
    SelectObject(wid_dc, wid_oldbm);

#else // use_GetPixel

    SelectObject(wid_dc, wid_oldbm);

# ifdef test_preload_bitmap_info
    BITMAP bm0;
    GetObject(wid_bm, sizeof(BITMAP), &bm0);
    //assuming:
    //bm0.bmWidthBytes == bm0.bmWidth * 4 == cell_width * 8
    //bm0.bmBitsPixel == 32
# endif
    BITMAPINFO bmi;
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
# ifdef test_precheck_bitmap
    int ok = GetDIBits(wid_dc, wid_bm, 0, cell_height, 0, &bmi, DIB_RGB_COLORS);
    printf("DI %d %d pl %d bt/px %d comp %d size %d\n",
           bmi.bmiHeader.biWidth, bmi.bmiHeader.biHeight,
           bmi.bmiHeader.biPlanes, bmi.bmiHeader.biBitCount,
           bmi.bmiHeader.biCompression, bmi.bmiHeader.biSizeImage);
    //assuming:
    //bmi.bmiHeader.biBitCount == 32
    //bmi.bmiHeader.biSizeImage == biWidth * biHeight * 4
# endif
    bmi.bmiHeader.biWidth = cell_width * 2;
    bmi.bmiHeader.biHeight = -cell_height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD * pixels = newn(DWORD, cell_width * 2 * cell_height);
    //int scanlines =
    GetDIBits(wid_dc, wid_bm, 0, cell_height, pixels, &bmi, DIB_RGB_COLORS);

# if defined(debug_win_char_width) && debug_win_char_width > 1
    for (int y = 0; y < cell_height; y++) {
      printf(" %2d|", y);
      for (int x = 0; x < cell_width * 2; x++) {
        COLORREF c = pixels[y * cell_width * 2 + x];
        printf("%c", c != RGB(0, 0, 0) ? '*' : ' ');
      }
      printf("|\n");
    }
# endif
    for (int x = cell_width * 2 - 1; !wid && x >= 0; x--)
      for (int y = 0; y < cell_height; y++) {
        COLORREF c = pixels[y * cell_width * 2 + x];
        if (c != RGB(0, 0, 0)) {
          wid = x + 1;
          break;
        }
      }

    free(pixels);

#endif // use_GetPixel

    DeleteObject(wid_bm);
    DeleteDC(wid_dc);

# ifdef debug_rendering
    clock_gettime(CLOCK_MONOTONIC, &tim);
    ulong then = tim.tv_sec * (long)1000000000 + tim.tv_nsec;
    printf("rendered %05X %s (t %ld) width %d -> %d\n", wc, attr & TATTR_WIDE ? "wide" : "narr", then - now, wid, wid > cell_width ? 2 : 1);
# endif

    return wid;
  }

  if ((c >= 0x2160 && c <= 0x2179)   // Roman Numerals
     )
  {
    ReleaseDC(wnd, dc);
    return 2;
  }
  if (c >= 0x2500 && c <= 0x257F) {  // Box Drawing
    ReleaseDC(wnd, dc);
    return 2;  // do not stretch; vertical lines might get pushed out of cell
  }
  if ((c >= 0x2580 && c <= 0x2588) || (c >= 0x2592 && c <= 0x2594)) {
    // Block Elements
    ReleaseDC(wnd, dc);
    return 1;  // should be stretched to fill whole cell
               // does not have the desired effect, 
               // although FONT_WIDE is actually activated
  }

  if ((c >= 0x3000 && c <= 0x303F)   // CJK Symbols and Punctuation

   || (c >= 0x01C4 && c <= 0x01CC)   // double letters
   || (c >= 0x01F1 && c <= 0x01F3)   // double letters

   || (c >= 0x2460 && c <= 0x24FF)   // Enclosed Alphanumerics, to cover:
   //|| (c >= 0x249C && c <= 0x24E9)   // parenthesized/circled letters

   //|| (c >= 0x2600 && c <= 0x27BF)   // Miscellaneous Symbols, Dingbats
   || c == 0x26AC

   || (c >= 0x3248 && c <= 0x324F)   // circled CJK Numbers
   || (c >= 0x1F100 && c <= 0x1F1FF) // Enclosed Alphanumeric Supplement

   || c == 0x2139                    // Letterlike: Information Source
   || (c >= 0x2180 && c <= 0x2182)   // Number Forms: combined Roman Numerals
   || (c >= 0x2187 && c <= 0x2188)   // Number Forms: combined Roman Numerals

   || (c >= 0xE000 && c < 0xF900)    // Private Use Area

   || (// check all non-letters with some exceptions
       bidi_class(c) != L               // indicates not a letter
      &&  // do not check these non-letters:
       !(  (c >= 0x2500 && c <= 0x2588)  // Box Drawing, Block Elements
        || (c >= 0x2591 && c <= 0x2594)  // Block Elements
        || (c >= 0x2160 && c <= 0x2179)  // Roman Numerals
        //|| wcschr (W("‐‑‘’‚‛“”„‟‹›"), c) // #712 workaround; now caching
        )
      )
     )
  {
    // look up c in charpropcache
    struct charpropcache * cpfound = 0;

    bool bold = (ff->bold_mode == BOLD_FONT) && (attr & ATTR_BOLD);
    bool italic = attr & ATTR_ITALIC;
    int font4index = (bold ? FONT_BOLD : 0) | (italic ? FONT_ITALIC : 0);

    for (uint i = 0; i < ff->cpcachelen[font4index]; i++)
      if (ff->cpcache[font4index][i].ch == c) {
        if (ff->cpcache[font4index][i].width) {
          ReleaseDC(wnd, dc);
          //printf(" win_char_width %04X (cache) -> %d\n", c, ff->cpcache[font4index][i].width);
          return ff->cpcache[font4index][i].width;
        }
        else {
          // cached (e.g. by win_check_glyphs) but not measured
          cpfound = &ff->cpcache[font4index][i];
        }
      }

    int mbuf = act_char_width(c);
    // report char as wide if its measured width is more than 1½ cells
    int width = mbuf > cell_width ? 2 : 1;
    ReleaseDC(wnd, dc);
# ifdef debug_win_char_width
    if (c > '~' || c == 'A') {
      printf(" measured %04X %dpx cell %dpx width %d\n", c, mbuf, cell_width, width);
    }
# endif
    // cache width
    if (cpfound)
      cpfound->width = width;
    else {
      // max size per cache 138739 as of Unicode 10.0;
      // we should perhaps limit this...
      struct charpropcache * newcpcache = renewn(ff->cpcache[font4index], ff->cpcachelen[font4index] + 1);
      if (newcpcache) {
        ff->cpcache[font4index] = newcpcache;
        ff->cpcache[font4index][ff->cpcachelen[font4index]].ch = c;
        ff->cpcache[font4index][ff->cpcachelen[font4index]].width = width;
        ff->cpcachelen[font4index]++;
      }
    }
    //printf(" win_char_width %04X -> %d\n", c, width);
    return width;
  }

#endif // measure_width

  ReleaseDC(wnd, dc);
  //printf(" win_char_width %04X -> %d\n", c, ibuf);
  return ibuf;
}

/*
 * Memoising wrapper around win_char_width_uncached; same contract:
 * return the width in character cells of code point c when rendered
 * with the font family/style selected by attributes attr (usually 1
 * or 2; 0 if width enquiry failed). Results are cached until the next
 * font (re)initialisation flushes the cache (win_init_fontfamily).
 */
int
win_char_width(xchar c, cattrflags attr)
{
  PERF_COUNT(wcw_calls, 1);
  if (c >= ' ' && c <= '~') {
    // ASCII fast path as in the uncached implementation;
    // keep these out of the cache
    PERF_COUNT(wcw_ascii_fast, 1);
    return 1;
  }
  if (c > 0x10FFFF) {
    // out of Unicode range; don't let it alias another cache key
    PERF_COUNT(wcw_out_of_range, 1);
    long long perf_t0 = mintty_perf_ticks();
    int wid = win_char_width_uncached(c, attr);
    PERF_ADD_TICKS(wcw_uncached_ticks, mintty_perf_ticks() - perf_t0);
    return wid;
  }
  uint key = wcw_key(c, attr);
  int wid;
  if (wcw_lookup(key, &wid)) {
    PERF_COUNT(wcw_cache_hits, 1);
    return wid;
  }
  PERF_COUNT(wcw_cache_misses, 1);
  long long perf_t0 = mintty_perf_ticks();
  wid = win_char_width_uncached(c, attr);
  PERF_ADD_TICKS(wcw_uncached_ticks, mintty_perf_ticks() - perf_t0);
  if (wid != 0) {
    // width 0 signals a failed width enquiry; do not memoise failures,
    // so they keep being retried per call as before
    wcw_store(key, wid);
  }
  return wid;
}

#define dont_debug_win_combine

/* Try to combine a base and combining character into a precomposed one.
 * Returns 0 if unsuccessful.
 */
wchar
win_combine_chars(wchar c, wchar cc, cattrflags attr)
{
  wchar cs[2];
  int len = FoldStringW(MAP_PRECOMPOSED, (wchar[]){c, cc}, 2, cs, 2);
  if (len == 1) {  // check whether the combined glyph exists
    int findex = (attr & FONTFAM_MASK) >> ATTR_FONTFAM_SHIFT;
    if (findex > 10)
      findex = 0;
    struct fontfam * ff = &fontfamilies[findex];

    HFONT f = font4(ff, attr);

    ushort glyph;
    HDC dc = GetDC(wnd);
    SelectObject(dc, f);
    GetGlyphIndicesW(dc, cs, 1, &glyph, true);
    ReleaseDC(wnd, dc);
#ifdef debug_win_combine
    printf("win_combine %04X %04X -> %04X\n", c, cc, glyph == 0xFFFF ? 0 : *cs);
#endif
    if (glyph == 0xFFFF)
      return 0;
    else
      return *cs;
  }
  else
    return 0;
}


// Colour settings

// Xterm256 colour cube and greyscale
static colour xterm_colours[240];

void
win_set_colour(colour_i i, colour c)
{
  if (i >= COLOUR_NUM)
    return;

  static bool bold_colour_selected = false;

  bool changed_something = false;
  void cc(colour_i i, colour c)
  {
    if (c != colours[i]) {
      colours[i] = c;
      changed_something = true;
    }
  }

  if (c == (colour)-1) {
    // ... reset to default ...
    if (i < 16) {
      colour_pair cp = cfg.ansi_colours[i];
      cc(i, cp.fg);
      cc(i + ANSI0, cp.fg);
      cc(i + BG_ANSI0, cp.bg);
    }
    else if (i < 256)
      cc(i, xterm_colours[i - 16]);
    else if (i < ANSI0 + 16)
      cc(i, cfg.ansi_colours[i - ANSI0].fg);
    else if (i < BG_ANSI0 + 16)
      cc(i, cfg.ansi_colours[i - BG_ANSI0].bg);
    else switch (i) {
      when BOLD_COLOUR_I: cc(BOLD_COLOUR_I, cfg.bold_colour);
      when BLINK_COLOUR_I: cc(BLINK_COLOUR_I, cfg.blink_colour);
      when BOLD_FG_COLOUR_I:
        bold_colour_selected = false;
        if (cfg.bold_colour != (colour)-1)
          cc(BOLD_FG_COLOUR_I, cfg.bold_colour);
        else
          cc(BOLD_FG_COLOUR_I,
             brighten(colours[FG_COLOUR_I], colours[BG_COLOUR_I], true));
      when FG_COLOUR_I: cc(i, cfg.fg_colour);
      when BG_COLOUR_I: cc(i, cfg.bg_colour);
      when CURSOR_COLOUR_I:
        cc(i, cfg.cursor_colour);
        if (cfg.ime_cursor_colour != DEFAULT_COLOUR)
          cc(IME_CURSOR_COLOUR_I, cfg.ime_cursor_colour);
        //printf("ime_cc set -1 %06X\n", cfg.ime_cursor_colour);
      when SEL_COLOUR_I: cc(i, cfg.sel_bg_colour);
      when SEL_TEXT_COLOUR_I: cc(i, cfg.sel_fg_colour);
      when TEK_FG_COLOUR_I: cc(i, cfg.tek_fg_colour);
      when TEK_BG_COLOUR_I: cc(i, cfg.tek_bg_colour);
      when TEK_CURSOR_COLOUR_I: cc(i, cfg.tek_cursor_colour);
      otherwise: ; // do nothing
    }
  }
  else {
#ifdef debug_brighten
    printf("colours[%d] = %06X\n", i, c);
#endif
    cc(i, c);
    if (i < 16) {
      cc(i + ANSI0, c);
      cc(i + BG_ANSI0, c);
    }
    else switch (i) {
      when FG_COLOUR_I:
        // should we make this conditional,
        // unless bold colour has been set explicitly?
        if (!bold_colour_selected) {
          if (cfg.bold_colour != (colour)-1)
            cc(BOLD_FG_COLOUR_I, cfg.bold_colour);
          else {
            cc(BOLD_FG_COLOUR_I, brighten(c, colours[BG_COLOUR_I], true));
            // renew this too as brighten() may refer to contrast colour:
            cc(BOLD_BG_COLOUR_I,
               brighten(colours[BG_COLOUR_I], colours[FG_COLOUR_I], true));
          }
        }
      when BOLD_FG_COLOUR_I:
        bold_colour_selected = true;
      when BG_COLOUR_I:
        if (!bold_colour_selected) {
          if (cfg.bold_colour != (colour)-1)
            cc(BOLD_FG_COLOUR_I, cfg.bold_colour);
          else {
            cc(BOLD_BG_COLOUR_I, brighten(c, colours[FG_COLOUR_I], true));
            // renew this too as brighten() may refer to contrast colour:
            cc(BOLD_FG_COLOUR_I,
               brighten(colours[FG_COLOUR_I], colours[BG_COLOUR_I], true));
          }
        }
      when CURSOR_COLOUR_I: {
        // Set the colour of text under the cursor to whichever of foreground
        // and background colour is further away from the cursor colour.
        colour fg = colours[FG_COLOUR_I], bg = colours[BG_COLOUR_I];
        colour _cc = colour_dist(c, fg) > colour_dist(c, bg) ? fg : bg;
        cc(CURSOR_TEXT_COLOUR_I, _cc);
        if (cfg.ime_cursor_colour != DEFAULT_COLOUR) {
          // effective IME cursor colour : configured IME cursor colour
          // = effective cursor colour : configured cursor colour
          // resp.
          // effective IME cursor colour : effective cursor colour
          // = configured IME cursor colour : configured cursor colour
          uint r = (uint)red(_cc) * (uint)red(cfg.ime_cursor_colour);
          if (red(cfg.cursor_colour))
            r /= red(cfg.cursor_colour);
          r = max(r, 255);
          uint g = (uint)green(_cc) * (uint)green(cfg.ime_cursor_colour);
          if (green(cfg.cursor_colour))
            g /= green(cfg.cursor_colour);
          g = max(r, 255);
          uint b = (uint)blue(_cc) * (uint)blue(cfg.ime_cursor_colour);
          if (blue(cfg.cursor_colour))
            b /= blue(cfg.cursor_colour);
          b = max(r, 255);
          c = RGB(r, g, b);
        }
        cc(IME_CURSOR_COLOUR_I, c);
        //printf("ime_cc set c %06X\n", c);
      }
      otherwise: ; // do nothing
    }
  }

  // Redraw everything.
  if (changed_something)
    win_invalidate_all(false);
}

colour
win_get_colour(colour_i i)
{
  if (term.rvideo && CCL_DEFAULT(i))
    return colours[i ^ 2];  // [BOLD]_FG_COLOUR_I  <-->  [BOLD]_BG_COLOUR_I
  return i < COLOUR_NUM ? colours[i] : 0;
}

void
win_reset_colours(void)
{
  // ANSI foreground and background colour variants.
  // The foreground variants are copied to the first 16 xterm256 slots.
  for (uint i = 0; i < 16; i++) {
    colours[ANSI0 + i] = colours[i] = cfg.ansi_colours[i].fg;
    colours[BG_ANSI0 + i] = cfg.ansi_colours[i].bg;
  }

  // Initialize Xterm256 colour cube and greyscale
  static bool xterm_colours_initialized = false;
  if (!xterm_colours_initialized) {
    xterm_colours_initialized = true;
    uint i = 0;
    for (uint r = 0; r < 6; r++)
      for (uint g = 0; g < 6; g++)
        for (uint b = 0; b < 6; b++)
          xterm_colours[i++] = RGB(r ? r * 40 + 55 : 0,
                                   g ? g * 40 + 55 : 0,
                                   b ? b * 40 + 55 : 0);
    for (uint s = 0; s < 24; s++) {
      uint c = s * 10 + 8;
      xterm_colours[i++] = RGB(c, c, c);
    }
  }

  memcpy(&colours[16], xterm_colours, sizeof xterm_colours);

  // Foreground, background, cursor
  win_set_colour(FG_COLOUR_I, cfg.fg_colour);
  win_set_colour(BG_COLOUR_I, cfg.bg_colour);
  win_set_colour(CURSOR_COLOUR_I, cfg.cursor_colour);
  if (cfg.ime_cursor_colour != DEFAULT_COLOUR) {
    win_set_colour(IME_CURSOR_COLOUR_I, cfg.ime_cursor_colour);
    //printf("ime_cc reset %06X\n", cfg.ime_cursor_colour);
  }
  win_set_colour(SEL_COLOUR_I, cfg.sel_bg_colour);
  win_set_colour(SEL_TEXT_COLOUR_I, cfg.sel_fg_colour);
  // attribute colours
  win_set_colour(BOLD_COLOUR_I, (colour)-1);
  win_set_colour(BLINK_COLOUR_I, (colour)-1);
#if defined(debug_bold) || defined(debug_brighten)
  string ci[] = {
    "FG_COLOUR_I", "BOLD_FG_COLOUR_I",
    "BG_COLOUR_I", "BOLD_BG_COLOUR_I",
    "CURSOR_TEXT_COLOUR_I", "CURSOR_COLOUR_I",
    "IME_CURSOR_COLOUR_I", "SEL_COLOUR_I",
    "SEL_TEXT_COLOUR_I", "BOLD_COLOUR_I"
  };
  for (int i = FG_COLOUR_I; i < COLOUR_NUM; i++)
    if (colours[i] == (colour)-1)
      printf("colour %d ------ [%s]\n", i, ci[i - FG_COLOUR_I]);
    else
      printf("colour %d %06X [%s]\n", i, (int)colours[i], ci[i - FG_COLOUR_I]);
#endif
  win_set_colour(TEK_FG_COLOUR_I, cfg.tek_fg_colour);
  win_set_colour(TEK_BG_COLOUR_I, cfg.tek_bg_colour);
  win_set_colour(TEK_CURSOR_COLOUR_I, cfg.tek_cursor_colour);
}


#define dont_debug_padding_background

void
win_paint(void)
{
  PAINTSTRUCT p;
  mintty_perf_update_begin("paint");
  PERF_SET(cols, term.cols);
  PERF_SET(rows, term.rows);
  PERF_SET(allrows, term_allrows);
  PERF_SET(cell_width, cell_width);
  PERF_SET(cell_height, cell_height);
  PERF_SET(display_speedup, cfg.display_speedup);
  PERF_SET(display_buffering, cfg.display_buffering);
  PERF_SET(ligatures, cfg.ligatures);
  PERF_SET(font_render, cfg.font_render);
  PERF_SET(disptop, term.disptop);
  long long perf_t0 = mintty_perf_ticks();
  dc = BeginPaint(wnd, &p);
  PERF_ADD_TICKS(beginpaint_ticks, mintty_perf_ticks() - perf_t0);

  // better invalidate more than less; limited to text area in term_invalidate
  term_invalidate(
    (p.rcPaint.left - PADDING) / cell_width,
    (p.rcPaint.top - PADDING - OFFSET) / cell_height,
    (p.rcPaint.right - PADDING - 1) / cell_width,
    (p.rcPaint.bottom - PADDING - OFFSET - 1) / cell_height
  );

  //if (kb_trace) printf("[%ld] win_paint state %d (idl/blk/pnd)\n", mtime(), update_state);
  if (update_state != UPDATE_PENDING) {
    if (tek_mode)
      tek_paint();
    else {
      perf_t0 = mintty_perf_ticks();
      bool buffered = win_paint_buffer_begin();
      PERF_ADD_TICKS(paint_buffer_begin_ticks, mintty_perf_ticks() - perf_t0);

      perf_t0 = mintty_perf_ticks();
      term_paint();
      PERF_ADD_TICKS(term_paint_ticks, mintty_perf_ticks() - perf_t0);

      if (buffered) {
        perf_t0 = mintty_perf_ticks();
        win_paint_buffer_end();
        PERF_ADD_TICKS(paint_buffer_end_ticks, mintty_perf_ticks() - perf_t0);
      }

      perf_t0 = mintty_perf_ticks();
      winimgs_paint();
      PERF_ADD_TICKS(winimgs_paint_ticks, mintty_perf_ticks() - perf_t0);
    }
  }

  long long perf_padding_t0 = mintty_perf_ticks();
  if (// check whether no background was configured and successfully loaded
      !bgbrush_bmp &&
#if CYGWIN_VERSION_API_MINOR >= 74
      !bgbrush_img &&
#endif
      // check whether we need to refresh padding background
      (p.fErase
       || p.rcPaint.left < PADDING
       || p.rcPaint.top < OFFSET + PADDING
       || p.rcPaint.right >= PADDING + cell_width * term.cols
       || p.rcPaint.bottom >= OFFSET + PADDING + cell_height * term_allrows
      )
     )
  {
    /* Notes:
       * Do we actually need this stuff? We paint the background with
         each win_text chunk anyway, except for the padding border,
         which could however be touched e.g. by Sixel images?
       * With a texture/image background, we could try to paint that here 
         (invoked on WM_PAINT) or on WM_ERASEBKGND, but these messages are 
         not received sufficiently often, e.g. not when scrolling.
       * So let's keep finer control and paint background with text chunks 
         but not modify the established behaviour if there is no background.
     */
    colour bg_colour = colours[term.rvideo ? FG_COLOUR_I : BG_COLOUR_I];
#ifdef debug_padding_background
    // visualize background for testing
    bg_colour = RGB(222, 0, 0);
#endif
    HBRUSH oldbrush = SelectObject(dc, CreateSolidBrush(bg_colour));
    HPEN oldpen = SelectObject(dc, CreatePen(PS_SOLID, 0, bg_colour));

    // unclear purpose
    IntersectClipRect(dc, p.rcPaint.left, p.rcPaint.top,
                          p.rcPaint.right, p.rcPaint.bottom);

    // mask inner area not to pad with background
    ExcludeClipRect(dc, PADDING,
                        OFFSET + PADDING,
                        PADDING + cell_width * term.cols,
                        OFFSET + PADDING + cell_height * term_allrows);

    // fill outer padding area with background
    int sy = win_search_visible() ? SEARCHBAR_HEIGHT : 0;
    Rectangle(dc, p.rcPaint.left, max(p.rcPaint.top, OFFSET),
                  p.rcPaint.right, p.rcPaint.bottom - sy);

    DeleteObject(SelectObject(dc, oldbrush));
    DeleteObject(SelectObject(dc, oldpen));
    // Padding was painted directly after any buffered terminal update.
    // Seed it back into the buffer before the next full-width row blit.
    paint_buf_stale = true;
#ifdef debug_padding_background
    // show visualized background for testing
    usleep(900000);
#endif
  }

  PERF_ADD_TICKS(padding_paint_ticks, mintty_perf_ticks() - perf_padding_t0);
  perf_t0 = mintty_perf_ticks();
  EndPaint(wnd, &p);
  PERF_ADD_TICKS(endpaint_ticks, mintty_perf_ticks() - perf_t0);
  mintty_perf_update_end();
}

