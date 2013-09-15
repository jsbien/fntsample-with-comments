/* Copyright © 2007-2010 Євгеній Мещеряков <eugen@debian.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TYPE1_TABLES_H
#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-ps.h>
#include <cairo-svg.h>
#include <cairo-ft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <stdint.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <libintl.h>
#include <locale.h>
#include <wchar.h>

#include "unicode_blocks.h"
#include "ucd_xml_reader.h"
#include "config.h"

#define _(str)	gettext(str)

#define A4_WIDTH	(8.3*72)
#define A4_HEIGHT	(11.7*72)

#define xmin_border	(72.0/1.5)
#define ymin_border	(72.0)
#define cell_width	((A4_WIDTH - 2*xmin_border) / 16)
#define cell_height	((A4_HEIGHT - 2*ymin_border) / 16)

#define CELL_X(x_min, N)	((x_min) + cell_width * ((N) / 16))
#define CELL_Y(N)	(ymin_border + cell_height * ((N) % 16))

static struct option longopts[] = { { "font-file", 1, 0, 'f' }, { "output-file",
    1, 0, 'o' }, { "help", 0, 0, 'h' }, { "other-font-file", 1, 0, 'd' }, {
    "postscript-output", 0, 0, 's' }, { "svg", 0, 0, 'g' }, { "print-outline",
    0, 0, 'l' }, { "include-range", 1, 0, 'i' }, { "exclude-range", 1, 0, 'x' },
    { "style", 1, 0, 't' }, { "font-index", 1, 0, 'n' }, { "other-index", 1, 0,
        'm' }, { "ucd-xml-file", 1, 0, 'r' }, { 0, 0, 0, 0 } };

struct range {
  uint32_t first;
  uint32_t last;
  bool include;
  struct range *next;
};

static const char *font_file_name;
static const char *other_font_file_name;
static const char *output_file_name;
static const char *xml_file_name = NULL;
static bool postscript_output;
static bool svg_output;
static bool print_outline;
static struct range *ranges;
static struct range *last_range;
static int font_index;
static int other_index;

struct fntsample_style {
  const char * const name;
  const char * const default_val;
  char *val;
};

static struct fntsample_style styles[] = {
    { "header-font", "Sans Bold 12", NULL }, { "font-name-font",
        "Serif Bold 12", NULL }, { "table-numbers-font", "Sans 10", NULL }, {
        "cell-numbers-font", "Mono 8", NULL }, { NULL, NULL, NULL } };

static PangoFontDescription *header_font;
static PangoFontDescription *font_name_font;
static PangoFontDescription *table_numbers_font;
static PangoFontDescription *cell_numbers_font;

static double cell_label_offset;
static double cell_glyph_bot_offset;
static double glyph_baseline_offset;

static void usage(const char *);

static struct fntsample_style *find_style(const char *name) {
  struct fntsample_style *style = styles;

  for (; style->name; style++) {
    if (!strcmp(name, style->name))
      return style;
  }

  return NULL;
}

static int set_style(const char *name, const char *val) {
  struct fntsample_style *style;
  char *new_val;

  style = find_style(name);

  if (!style)
    return -1;

  new_val = strdup(val);
  if (!new_val)
    return -1;

  if (style->val)
    free(style->val);
  style->val = new_val;

  return 0;
}

static const char *get_style(const char *name) {
  struct fntsample_style *style = find_style(name);

  if (!style)
    return NULL;

  return style->val ? style->val : style->default_val;
}

static int parse_style_string(char *s) {
  char *n;

  n = strchr(s, ':');
  if (!n)
    return -1;
  *n++ = '\0';
  return set_style(s, n);
}

/*
 * Update output range.
 *
 * Returns -1 on error.
 */
static int add_range(char *range, bool include) {
  struct range *r;
  uint32_t first = 0, last = 0xffffffff;
  char *minus;
  char *endptr;

  minus = strchr(range, '-');

  if (minus) {
    if (minus != range) {
      *minus = '\0';
      first = strtoul(range, &endptr, 0);
      if (*endptr)
        return -1;
    }

    if (*(minus + 1)) {
      last = strtoul(minus + 1, &endptr, 0);
      if (*endptr)
        return -1;
    }
    else if (minus == range)
      return -1;
  }
  else {
    first = strtoul(range, &endptr, 0);
    if (*endptr)
      return -1;
    last = first;
  }

  if (first > last)
    return -1;

  r = malloc(sizeof(*r));
  if (!r)
    return -1;

  r->first = first;
  r->last = last;
  r->include = include;
  r->next = NULL;

  if (ranges)
    last_range->next = r;
  else ranges = r;

  last_range = r;

  return 0;
}

/*
 * Check if character with the given code belongs
 * to output range specified by the user.
 */
static bool in_range(uint32_t c) {
  bool in = ranges ? (!ranges->include) : 1;
  struct range *r;

  for (r = ranges; r; r = r->next) {
    if ((c >= r->first) && (c <= r->last))
      in = r->include;
  }
  return in;
}

/*
 * Get glyph index for the next glyph from the given font face, that
 * represents character from output range specified by the user.
 *
 * Returns character code, updates 'idx'.
 * 'idx' can became 0 if there are no more glyphs.
 */
static FT_ULong get_next_char(FT_Face face, FT_ULong charcode, FT_UInt *idx) {
  FT_ULong rval = charcode;

  do {
    rval = FT_Get_Next_Char(face, rval, idx);
  } while (*idx && !in_range(rval));

  return rval;
}

/*
 * Locate first character from the given font face that belongs
 * to the user-specified output range.
 *
 * Returns character code, updates 'idx' with glyph index.
 * Glyph index can became 0 if there are no matching glyphs in the font.
 */
static FT_ULong get_first_char(FT_Face face, FT_UInt *idx) {
  FT_ULong rval;

  rval = FT_Get_First_Char(face, idx);

  if (*idx && !in_range(rval))
    rval = get_next_char(face, rval, idx);

  return rval;
}

/*
 * Create Pango layout for the given text.
 * Updates 'r' with text extents.
 * Returned layout should be freed using g_object_unref().
 */
static PangoLayout *layout_text(cairo_t *cr, PangoFontDescription *ftdesc,
    const char *text, PangoRectangle *r) {
  PangoLayout *layout;

  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, ftdesc);
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_extents(layout, r, NULL);

  return layout;
}

static void parse_options(int argc, char * const argv[]) {
  for (;;) {
    int c;

    c = getopt_long(argc, argv, "f:o:hd:sgli:x:t:n:m:r:", longopts, NULL);

    if (c == -1)
      break;

    switch (c) {
      case 'f':
        if (font_file_name) {
          fprintf(stderr, _("Font file name should be given only once!\n"));
          exit(1);
        }
        font_file_name = optarg;
        break;
      case 'o':
        if (output_file_name) {
          fprintf(stderr, _("Output file name should be given only once!\n"));
          exit(1);
        }
        output_file_name = optarg;
        break;
      case 'h':
        usage(argv[0]);
        exit(0);
        break;
      case 'd':
        if (other_font_file_name) {
          fprintf(stderr, _("Font file name should be given only once!\n"));
          exit(1);
        }
        other_font_file_name = optarg;
        break;
      case 's':
        postscript_output = true;
        break;
      case 'g':
        svg_output = true;
        break;
      case 'l':
        print_outline = true;
        break;
      case 'i':
      case 'x':
        if (add_range(optarg, c == 'i')) {
          usage(argv[0]);
          exit(1);
        }
        break;
      case 't':
        if (parse_style_string(optarg) == -1) {
          usage(argv[0]);
          exit(1);
        }
        break;
      case 'n':
        font_index = atoi(optarg);
        break;
      case 'm':
        other_index = atoi(optarg);
        break;
      case 'r':
        if (xml_file_name) {
          fprintf(stderr, _("XML file name should be given only once!\n"));
          exit(1);
        }
        xml_file_name = optarg;
        break;
      case '?':
      default:
        usage(argv[0]);
        exit(1);
        break;
    }
  }
  if (!font_file_name || !output_file_name) {
    usage(argv[0]);
    exit(1);
  }
  if (font_index < 0 || other_index < 0) {
    fprintf(stderr, _("Font index should be non-negative!\n"));
    exit(1);
  }
  if (postscript_output && svg_output) {
    fprintf(stderr, _("-s and -g cannot be used together!\n"));
    exit(1);
  }
}

/*
 * Locate unicode block that contains given character code.
 * Returns this block or NULL if not found.
 */
static const struct unicode_block *get_unicode_block(unsigned long charcode) {
  const struct unicode_block *block;

  for (block = unicode_blocks; block->name; block++) {
    if ((charcode >= block->start) && (charcode <= block->end))
      return block;
  }
  return NULL;
}

/*
 * Check if the given character code belongs to the given Unicode block.
 */
static bool is_in_block(unsigned long charcode,
    const struct unicode_block *block) {
  return ((charcode >= block->start) && (charcode <= block->end));
}

/*
 * Format and print outline information, if requested by the user.
 */
static void outline(int level, int page, const char *text) {
  if (print_outline)
    printf("%d %d %s\n", level, page, text);
}

/*
 * Draw header of a page.
 * Header shows font name and current Unicode block.
 */
static void draw_header(cairo_t *cr, const char *face_name,
    const char *block_name) {
  PangoLayout *layout;
  PangoRectangle r;

  layout = layout_text(cr, font_name_font, face_name, &r);
  cairo_move_to(cr, (A4_WIDTH - (double) r.width / PANGO_SCALE) / 2.0, 30.0);
  pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
  g_object_unref(layout);

  layout = layout_text(cr, header_font, block_name, &r);
  cairo_move_to(cr, (A4_WIDTH - (double) r.width / PANGO_SCALE) / 2.0, 50.0);
  pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
  g_object_unref(layout);
}

/*
 * Highlight the cell with given coordinates.
 * Used to highlight new glyphs.
 */
static void highlight_cell(cairo_t *cr, double x, double y) {
  cairo_save(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 0.6);
  cairo_rectangle(cr, x, y, cell_width, cell_height);
  cairo_fill(cr);
  cairo_restore(cr);
}

/*
 * Try to place glyph with the given index at the middle of the cell.
 * Changes argument 'glyph'
 */
static void position_glyph(cairo_t *cr, double x, double y, unsigned long idx,
    cairo_glyph_t *glyph) {
  cairo_text_extents_t extents;

  *glyph = (cairo_glyph_t) {idx, 0, 0};

  cairo_glyph_extents(cr, glyph, 1, &extents);

  glyph->x += x + (cell_width - extents.width) / 2.0 - extents.x_bearing;
  glyph->y += y + glyph_baseline_offset;
}

/*
 * Draw table grid with row and column numbers.
 */
static void draw_grid(cairo_t *cr, unsigned int x_cells,
    unsigned long block_start) {
  unsigned int i;
  double x_min = (A4_WIDTH - x_cells * cell_width) / 2;
  double x_max = (A4_WIDTH + x_cells * cell_width) / 2;
  char buf[9];
  PangoLayout *layout;
  PangoRectangle r;

#define TABLE_H (A4_HEIGHT - ymin_border * 2)
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, x_min, ymin_border, x_max - x_min, TABLE_H);
  cairo_move_to(cr, x_min, ymin_border);
  cairo_line_to(cr, x_min, ymin_border - 15.0);
  cairo_move_to(cr, x_max, ymin_border);
  cairo_line_to(cr, x_max, ymin_border - 15.0);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 0.5);
  /* draw horizontal lines */
  for (i = 1; i < 16; i++) {
    cairo_move_to(cr, x_min, 72.0 + i * TABLE_H / 16);
    cairo_line_to(cr, x_max, 72.0 + i * TABLE_H / 16);
  }

  /* draw vertical lines */
  for (i = 1; i < x_cells; i++) {
    cairo_move_to(cr, x_min + i * cell_width, ymin_border);
    cairo_line_to(cr, x_min + i * cell_width, A4_HEIGHT - ymin_border);
  }
  cairo_stroke(cr);

  /* draw glyph numbers */
  buf[1] = '\0';
#define hexdigs	"0123456789ABCDEF"

  for (i = 0; i < 16; i++) {
    buf[0] = hexdigs[i];
    layout = layout_text(cr, table_numbers_font, buf, &r);
    cairo_move_to(cr, x_min - (double) PANGO_RBEARING(r) / PANGO_SCALE - 5.0,
        72.0 + (i + 0.5) * TABLE_H / 16
            + (double) PANGO_DESCENT(r) / PANGO_SCALE / 2);
    pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
    cairo_move_to(cr, x_min + x_cells * cell_width + 5.0,
        72.0 + (i + 0.5) * TABLE_H / 16
            + (double) PANGO_DESCENT(r) / PANGO_SCALE / 2);
    pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
    g_object_unref(layout);
  }

  for (i = 0; i < x_cells; i++) {
    snprintf(buf, sizeof(buf), "%03lX", block_start / 16 + i);
    layout = layout_text(cr, table_numbers_font, buf, &r);
    cairo_move_to(cr,
        x_min + i * cell_width
            + (cell_width - (double) r.width / PANGO_SCALE) / 2,
        ymin_border - 5.0);
    pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
    g_object_unref(layout);
  }
}

/*
 * Fill empty cell. Color of the fill depends on the character properties.
 */
static void fill_empty_cell(cairo_t *cr, double x, double y,
    unsigned long charcode) {
  cairo_save(cr);
  if (g_unichar_isdefined(charcode)) {
    if (g_unichar_iscntrl(charcode))
      cairo_set_source_rgb(cr, 0.0, 0.0, 0.5);
    else cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
  }
  cairo_rectangle(cr, x, y, cell_width, cell_height);
  cairo_fill(cr);
  cairo_restore(cr);
}

/*
 * Draw label with character code.
 */
static void draw_charcode(cairo_t *cr, double x, double y, FT_ULong charcode) {
  char buf[9];
  PangoLayout *layout;
  PangoRectangle r;

  snprintf(buf, sizeof(buf), "%04lX", charcode);
  layout = layout_text(cr, cell_numbers_font, buf, &r);
  cairo_move_to(cr, x + (cell_width - (double) r.width / PANGO_SCALE) / 2.0,
      y + cell_height - cell_label_offset);
  pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, 0));
  g_object_unref(layout);
}

/*
 * Draws tables for all characters in the given Unicode block.
 * Use font described by face and ft_face. Start from character
 * with given charcode (it should belong to the given Unicode
 * block). After return 'charcode' equals the last character code
 * of the block.
 *
 * Returns number of pages drawn.
 */
static int draw_unicode_block(cairo_t *cr, cairo_scaled_font_t *font,
    FT_Face ft_face, const char *fontname, unsigned long *charcode,
    const struct unicode_block *block, FT_Face ft_other_face) {
  FT_UInt idx;
  unsigned long prev_charcode;
  unsigned long prev_cell;
  int npages = 0;

  idx = FT_Get_Char_Index(ft_face, *charcode);

  do {
    unsigned long offset = ((*charcode - block->start) / 0x100) * 0x100;
    unsigned long tbl_start = block->start + offset;
    unsigned long tbl_end =
        tbl_start + 0xFF > block->end ? block->end + 1 : tbl_start + 0x100;
    unsigned int rows = (tbl_end - tbl_start) / 16;
    double x_min = (A4_WIDTH - rows * cell_width) / 2;
    unsigned long i;
    bool filled_cells[256]; /* 16x16 glyphs max */

    /* XXX WARNING: not reentrant! */
    static cairo_glyph_t glyphs[256];
    unsigned int nglyphs = 0;

    cairo_save(cr);
    draw_header(cr, fontname, block->name);
    prev_cell = tbl_start - 1;

    memset(filled_cells, '\0', sizeof(filled_cells));

    cairo_set_scaled_font(cr, font);
    /*
     * Fill empty cells and calculate coordinates of the glyphs.
     * Also highlight cells if needed.
     */
    do {
      /* the current glyph position in the table */
      int charpos = *charcode - tbl_start;

      /* fill empty cells before the current glyph */
      for (i = prev_cell + 1; i < *charcode; i++) {
        int pos = i - tbl_start;
        fill_empty_cell(cr, CELL_X(x_min, pos), CELL_Y(pos), i);
      }

      /* if it is new glyph - highlight the cell */
      if (ft_other_face && !FT_Get_Char_Index(ft_other_face, *charcode))
        highlight_cell(cr, CELL_X(x_min, charpos), CELL_Y(charpos));

      /* For now just position glyphs. They will be shown later,
       * to make output more efficient. */
      position_glyph(cr, CELL_X(x_min, charpos), CELL_Y(charpos), idx,
          &glyphs[nglyphs++]);

      filled_cells[charpos] = true;

      prev_charcode = *charcode;

      prev_cell = *charcode;
      *charcode = get_next_char(ft_face, *charcode, &idx);
    } while (idx && (*charcode < tbl_end) && is_in_block(*charcode, block));

    /* Fill remaining empty cells */
    for (i = prev_cell + 1; i < tbl_end; i++) {
      int pos = i - tbl_start;
      fill_empty_cell(cr, CELL_X(x_min, pos), CELL_Y(pos), i);
    }

    /* Show previously positioned glyphs */
    cairo_show_glyphs(cr, glyphs, nglyphs);

    for (i = 0; i < tbl_end - tbl_start; i++)
      if (filled_cells[i])
        draw_charcode(cr, CELL_X(x_min, i), CELL_Y(i), i + tbl_start);

    draw_grid(cr, rows, tbl_start);
    npages++;
    cairo_show_page(cr);
    cairo_restore(cr);
  } while (idx && is_in_block(*charcode, block));

  *charcode = prev_charcode;
  return npages;
}

/* =================================================================================== */
/* Draw UCD comments.
 *
 * Author: Paweł Parafiński <ppablo28@gmail.com>
 * This is an extension of the project available at: http://sourceforge.net/projects/fntsample/?source=dlp
 * In this version UCD comments are drawn after each block.
 */
/* =================================================================================== */

#define BLOCK_HEADER_X(hWidth)  (A4_WIDTH - hWidth / PANGO_SCALE) / 2.0
#define BLOCK_HEADER_Y  30.0
#define COLUMN_WIDTH    A4_WIDTH / 2.0
#define MAX_COLUMN_Y    A4_HEIGHT - ymin_border
#define BASE_Y          BLOCK_HEADER_Y + 10.0
#define OFFSET_BASE     10.0
#define OFFSET_SPACE    5.0
#define COORD_X(factor) xmin_border + OFFSET_BASE + factor * (COLUMN_WIDTH - xmin_border - OFFSET_BASE)
#define WRAP_LIMIT(val) (COLUMN_WIDTH - val) * PANGO_SCALE
#define RES_FACTOR      96.0 / 72.0

/* UTF-8 chars to distinguish different UCD data */
const unsigned char rightwards_arrow[] = { 0xE2, 0x86, 0x92, 0x0 };
const unsigned char bullet[] = { 0xE2, 0x80, 0xA2, 0x0 };
const unsigned char equal_sign[] = { 0x3D, 0x0 };
const unsigned char asterisk[] = { 0x2A, 0x0 };
const unsigned char reference_mark[] = { 0xE2, 0x80, 0xBB, 0x0 };
const unsigned char approx[] = { 0xE2, 0x89, 0x88, 0x0 };
const unsigned char equiv[] = { 0xE2, 0x89, 0xA1, 0x0 };
const unsigned char tilde[] = { 0x7E, 0x0 };
const unsigned char less_than[] = { 0x3C, 0x0 };
const unsigned char greater_than[] = { 0x3E, 0x0 };

/* The root of the block headers */
static struct header_block *ucd_blocks = NULL;

/* A set of fonts for drawing UCD data */
static PangoFontDescription *notice_line_font;
static PangoFontDescription *block_header_font;
static PangoFontDescription *subheader_font;
static PangoFontDescription *other_font;

struct ucd_style {
  const char * const name;
  const char * const style;
};

static struct ucd_style ucd_styles[] = { { "notice_line", "Sans Italic 6" }, {
    "block_header", "Sans Bold 11" }, { "block_subheader", "Sans Bold 9" }, {
    "other", "Serif 7" }, { NULL, NULL } };

static const char const *get_ucd_style(const char *name) {
  struct ucd_style *style = ucd_styles;

  for (; style->name; style++) {
    if (!strcmp(name, style->name))
      return style->style;
  }

  return NULL;
}

/* Initialize all fonts needed for generating UCD comments */
static void init_ucd_fonts(void) {
  block_header_font = pango_font_description_from_string(
      get_ucd_style("block_header"));
  subheader_font = pango_font_description_from_string(
      get_ucd_style("block_subheader"));
  notice_line_font = pango_font_description_from_string(
      get_ucd_style("notice_line"));
  other_font = pango_font_description_from_string(get_ucd_style("other"));
}

/* Get the height of the given layout and delete it */
static double get_pango_layout_height_and_free(PangoLayout *layout) {
  int height;
  pango_layout_get_size(layout, NULL, &height);
  g_object_unref(layout);
  return (double) height / PANGO_SCALE;
}

/* Get the width of the given layout and delete it */
static double get_pango_layout_width_and_free(PangoLayout *layout) {
  int width;
  pango_layout_get_size(layout, &width, NULL);
  g_object_unref(layout);
  return (double) width / PANGO_SCALE;
}

/* Draw basic text in the layout with the given font and wrap it (optional) */
static PangoLayout *draw_ucd_text(cairo_t *cr, const char *text,
    PangoFontDescription *font, int wrap_width) {
  PangoLayout *layout = layout_text(cr, font, text, NULL);
  pango_layout_set_width(layout,
      wrap_width != -1.0 ? WRAP_LIMIT(wrap_width) : -1.0);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
  pango_cairo_show_layout(cr, layout);
  return layout;
}

/*
 * Draw header of the UCD block.
 */
static PangoLayout *draw_ucd_block_header(cairo_t *cr, const char *block_header_name) {
  PangoLayout *layout;
  PangoRectangle r;

  layout = layout_text(cr, block_header_font, block_header_name, &r);
  cairo_move_to(cr, BLOCK_HEADER_X((double) r.width), BLOCK_HEADER_Y);
  pango_cairo_show_layout(cr, layout);
  return layout;
}

/* Draw single UCD char code using given font and coordinates */
static PangoLayout *draw_ucd_charcode(cairo_t *cr, PangoFontDescription *font,
    FT_ULong charcode, double x, double y) {
  char code[8];

  /* Draw the char code (fill with '0' if less than 4 signs) */
  cairo_move_to(cr, x, y);
  sprintf(code, "%04lX", charcode);
  return draw_ucd_text(cr, code, font, -1.0);
}

/* Draw the first and the last character code drawn at the current page */
static void draw_ucd_char_limits(cairo_t *cr, FT_ULong leftLimit,
    FT_ULong rightLimit) {
  PangoLayout *layout;
  double width;

  /* Draw left char code and get its width */
  layout = draw_ucd_charcode(cr, block_header_font, leftLimit, xmin_border,
      BLOCK_HEADER_Y);
  width = get_pango_layout_width_and_free(layout);

  /* Draw right char code */
  layout =
      draw_ucd_charcode(cr, block_header_font, rightLimit,
          A4_WIDTH - xmin_border - width, BLOCK_HEADER_Y);
  g_object_unref(layout);
}

/*
 * Draw properties of character entries, subheaders or blocks
 */
static PangoLayout *draw_ucd_tag(cairo_t *cr,
    const struct simple_tag * const tag, double x, double y, double offset) {
  PangoLayout *layout = NULL;
  double width;

  /* Move to the proper place */
  cairo_move_to(cr, x, y);

  /* NOTICE LINE */
  if (strcmp(tag->name, xmlTags.NOTICE_LINE) == 0) {
    char *text = NULL;

    /* If the notice line has any additional attributes - check them */
    if (tag->info) {
      struct tag_attr *attr = tag->info;
      for (; attr; attr = attr->next) {
        if (strcmp(attr->name, xmlAttrs.WITH_ASTERISK) == 0) {
          text = malloc(snprintf(NULL, 0, "%s %s", asterisk, tag->content) + 1);
          sprintf(text, "%s %s", asterisk, tag->content);
        }
      }
    }

    /* Draw text, free memory if needed */
    if (text) {
      layout = draw_ucd_text(cr, text, notice_line_font,
          xmin_border + OFFSET_BASE);
      free(text);
    }
    else {
      layout = draw_ucd_text(cr, tag->content, notice_line_font,
          xmin_border + OFFSET_BASE);
    }

    return layout;
  }

  /* At first draw a sign, then draw the content (take care of the sign width) */

  /* COMMENT LINE */
  if (strcmp(tag->name, xmlTags.COMMENT_LINE) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", bullet) + 1);
    sprintf(text, "%s ", bullet);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* ALIAS LINE */
  if (strcmp(tag->name, xmlTags.ALIAS_LINE) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", equal_sign) + 1);
    sprintf(text, "%s ", equal_sign);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* CROSS REF */
  if (strcmp(tag->name, xmlTags.CROSS_REF) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", rightwards_arrow) + 1);
    sprintf(text, "%s ", rightwards_arrow);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* COMPAT MAPPING */
  if (strcmp(tag->name, xmlTags.COMPAT_MAPPING) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", approx) + 1);
    sprintf(text, "%s ", approx);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* VARIATION LINE */
  if (strcmp(tag->name, xmlTags.VARIATION_LINE) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", tilde) + 1);
    sprintf(text, "%s ", tilde);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* DECOMPOSITION */
  if (strcmp(tag->name, xmlTags.DECOMPOSITION) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", equiv) + 1);
    sprintf(text, "%s ", equiv);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  /* FORMAL ALIAS LINE */
  if (strcmp(tag->name, xmlTags.FORMALALIAS_LINE) == 0) {
    char *text = malloc(snprintf(NULL, 0, "%s ", reference_mark) + 1);
    sprintf(text, "%s ", reference_mark);
    layout = draw_ucd_text(cr, text, other_font, -1.0);
    free(text);
  }

  width = get_pango_layout_width_and_free(layout);
  cairo_move_to(cr, x + width, y);
  layout = draw_ucd_text(cr, tag->content, other_font,
      xmin_border + OFFSET_BASE + offset + width);

  return layout;
}

/*
 * Check if the current 'y' coordinate is correct (in the text drawing area). If not
 * change the column, where the text is drawn (optionally create new page). Draw the first
 * and the last char together with page change. Draw current block header as the first
 * element of the new page. Reset 'y' coordinate and update column  current indicator.
 */
static void check_and_update_coords(cairo_t *cr, double *factor, double *coordY,
    const struct header_block *block, FT_ULong *firstChar, FT_ULong *lastChar) {
  /* If the max coordinate for a column encountered show new page and update proper values */
  if (*coordY >= MAX_COLUMN_Y) {
    if (*factor == 1.0) {
      /* Draw code points of the first and the last character drawn at this page */
      if (*firstChar != -1UL && *lastChar != -1UL) {
        draw_ucd_char_limits(cr, *firstChar, *lastChar);
        *firstChar = -1UL, *lastChar = -1UL;
      }

      /* Show the next page */
      cairo_show_page(cr);
    }

    /* Update the factor and y coordinate */
    *factor = abs(*factor - 1.0);
    *coordY = BASE_Y;
  }

  /* If new page has been drawn show header and limits */
  if (*coordY == BASE_Y) {
    /* If new page added - draw the header's name and code points limits */
    double height = get_pango_layout_height_and_free(
        draw_ucd_block_header(cr, block->name));

    /* Update the y coordinate */
    *coordY += height;
  }
}

/* Draw simple tags (notice lines, cross references, etc.). Update all variables during drawing.
 *
 * Parameters:
 *   multFactor - whether draw the text in the first column (0.0) or in the second one (1.0)
 *   x - the offset of the x coordinate
 *   y - the y coordinate
 *   first - the first drawn character at this page
 *   last - the last drawn character at this page
 *   bl - the header block (the current one)
 */
static void draw_ucd_simple_tags(cairo_t *cr, struct simple_tag *tags,
    double *multFactor, double x, double *y, FT_ULong *first, FT_ULong *last,
    const struct header_block *bl) {
  PangoLayout *layout;
  const struct simple_tag *smpl_tags;

  for (smpl_tags = tags; smpl_tags; smpl_tags = smpl_tags->next) {
    check_and_update_coords(cr, multFactor, y, bl, first, last);
    layout = draw_ucd_tag(cr, smpl_tags, COORD_X(*multFactor) + x, *y, x);
    if (layout != NULL) {
      *y += get_pango_layout_height_and_free(layout);
    }
  }
}

/* Draw a char entry (code point, char, name)
 *
 * Parameters:
 *   multFactor - whether draw the text in the first column (0.0) or in the second one (1.0)
 *   y - the y coordinate
 *   first - the first drawn character at this page
 *   last - the last drawn character at this page
 *   block - the header block (the current one)
 */
static void draw_ucd_char_entry(cairo_t *cr, FT_Face ft_face,
    cairo_scaled_font_t *font, const struct char_entry *entry,
    double *multFactor, double *width, double *coordY, FT_ULong *first,
    FT_ULong *last, const struct header_block *block) {
  PangoLayout *layout;
  double temp_width, text_height;
  int tempH;
  FT_UInt idx = FT_Get_Char_Index(ft_face, (FT_ULong) entry->cp);
  cairo_glyph_t glyphs[1];
  cairo_matrix_t matrix;
  cairo_font_extents_t extents;

  // Draw charcode
  check_and_update_coords(cr, multFactor, coordY, block, first, last);
  layout = draw_ucd_charcode(cr, other_font, entry->cp, COORD_X(*multFactor),
      *coordY);

  // Get the height of the comments' text
  pango_layout_get_size(layout, NULL, &tempH);
  text_height = ((double) tempH) / PANGO_SCALE;
  text_height = text_height * RES_FACTOR;
  // Get the width and release the layout
  temp_width = get_pango_layout_width_and_free(layout);

  if (entry->name) {
    cairo_save(cr);
    cairo_set_scaled_font(cr, font);
    cairo_get_font_matrix(cr, &matrix);
    cairo_font_extents(cr, &extents);
    cairo_matrix_scale(&matrix, text_height / extents.height,
        text_height / extents.height);
    cairo_set_font_matrix(cr, &matrix);

    /* Try to draw sign */
    glyphs[0] =
        (cairo_glyph_t) {idx, COORD_X(*multFactor) + OFFSET_SPACE + temp_width, *coordY + text_height / 2.0};
    cairo_show_glyphs(cr, glyphs, 1);
    *width = 2.0 * OFFSET_SPACE + temp_width;

    // Show the name of the char
    cairo_move_to(cr, COORD_X(*multFactor) + OFFSET_SPACE + *width, *coordY);
    layout = draw_ucd_text(cr, entry->name, other_font,
        xmin_border + OFFSET_BASE);
    *width = 2.0 * OFFSET_SPACE + *width;
    cairo_restore(cr);
  }
  else {
    *width = temp_width;
    cairo_move_to(cr, COORD_X(*multFactor) + OFFSET_SPACE + *width, *coordY);
    char *text = malloc(
        snprintf(NULL, 0, "%s%s%s", less_than, entry->type, greater_than) + 1);
    sprintf(text, "%s%s%s", less_than, entry->type, greater_than);
    layout = draw_ucd_text(cr, text, other_font,
        xmin_border + OFFSET_BASE + OFFSET_SPACE);
    *width = OFFSET_SPACE + *width;
    free(text);
  }
  *coordY += get_pango_layout_height_and_free(layout);
}

static int glyphs_can_be_drawn(FT_Face ft_face, struct char_entry *entry) {
  struct char_entry *temp = entry;
  for (; temp; temp = temp->next) {
    if ((FT_Get_Char_Index(ft_face, (FT_ULong) temp->cp) && temp->name) || !temp->name) {
      return 0;
    }
  }
  return 1;
}

/*
 * The main function of drawing UCD comments. It takes the character code and depending
 * on the value chooses the actual block header.
 */
static void draw_ucd_data(cairo_t *cr, FT_Face ft_face,
    cairo_scaled_font_t *font, const FT_ULong charcode) {
  PangoLayout *layout;
  double height = 0.0, width = 0.0;
  double multFactor = 0.0; /* Draw text in the first column (0.0) or the second one (1.0) */
  double coordY = BASE_Y; /* Coordinate Y */

  /* The first and the last drawn character code */
  FT_ULong drawnFirst = -1UL;
  FT_ULong drawnLast = -1UL;

  /* Get the block containing the given character */
  const struct header_block *block = find_ucd_block(ucd_blocks, charcode);
  const struct subheader_block *sub_block;
  const struct char_entry *entry;

  if (block) {
    /* Draw all tags connected with this block header (notice lines, cross references, etc.) */
    draw_ucd_simple_tags(cr, block->outer_tags, &multFactor, 0.0, &coordY,
        &drawnFirst, &drawnLast, block);

    for (sub_block = block->subheaders; sub_block; sub_block =
        sub_block->next) {
      /* Do not draw comment if not in range */
      if ((!in_range(sub_block->start) && !in_range(sub_block->end)) || glyphs_can_be_drawn(ft_face, sub_block->chars) == 1)
        continue;

      /* Draw subheader name and update the 'y' coordinate */
      check_and_update_coords(cr, &multFactor, &coordY, block, &drawnFirst,
          &drawnLast);
      cairo_move_to(cr, COORD_X(multFactor), coordY);
      layout = draw_ucd_text(cr, sub_block->name, subheader_font,
          xmin_border + OFFSET_BASE);
      height = get_pango_layout_height_and_free(layout);
      coordY += height + 1.0;

      /* Draw all tags connected with this subheader (notice lines, cross references, etc.) */
      draw_ucd_simple_tags(cr, sub_block->outer_tags, &multFactor, 0.0, &coordY,
          &drawnFirst, &drawnLast, block);

      /* Draw all char entries from this block */
      for (entry = sub_block->chars; entry; entry = entry->next) {
        /* Do not draw comment if not in range */
        if (!in_range(entry->cp) || (!FT_Get_Char_Index(ft_face, (FT_ULong) entry->cp) && entry->name))
          continue;

        draw_ucd_char_entry(cr, ft_face, font, entry, &multFactor, &width,
            &coordY, &drawnFirst, &drawnLast, block);

        /* Update values of the first char (only at the beginning) and the last one (always) */
        if (drawnFirst == -1UL) {
          drawnFirst = entry->cp;
        }
        drawnLast = entry->cp;

        /* Draw all information connected with this char entry */
        draw_ucd_simple_tags(cr, entry->char_info, &multFactor, width, &coordY,
            &drawnFirst, &drawnLast, block);
      }
    }
    /* Drawing ended before creating a new page */
    draw_ucd_char_limits(cr, drawnFirst, drawnLast);

    /* Drawing ended - show new page */
    cairo_show_page(cr);
  }
}

/*
 * The main drawing function.
 */
static void draw_glyphs(cairo_t *cr, cairo_scaled_font_t *font, FT_Face ft_face,
    const char *fontname, FT_Face ft_other_face) {
  FT_ULong charcode;
  FT_UInt idx;
  const struct unicode_block *block;
  int pageno = 1;

  /* Prepare to drawing comments if program argument's been specified */
  if (xml_file_name) {
    xmlDoc *doc = NULL;
    xmlNode *root_element = NULL;

    LIBXML_TEST_VERSION

    /* Parse the file and get the DOM */
    doc = xmlReadFile(xml_file_name, NULL, 0);
    if (doc == NULL) {
      printf("error: could not parse file %s\n", xml_file_name);
    }

    /*Get the root element node, parse and free the whole structure */
    root_element = xmlDocGetRootElement(doc);
    ucd_blocks = parse_ucd_from_xml(root_element->children);
    xmlFreeDoc(doc);

    /* Initialize necessary fonts */
    init_ucd_fonts();
  }

  outline(0, pageno, fontname);

  charcode = get_first_char(ft_face, &idx);

  while (idx) {
    block = get_unicode_block(charcode);
    if (block) {
      int npages;
      outline(1, pageno, block->name);
      npages = draw_unicode_block(cr, font, ft_face, fontname, &charcode, block,
          ft_other_face);
      pageno += npages;

      /* Draw comments */
      if (xml_file_name && ucd_blocks) {
        draw_ucd_data(cr, ft_face, font, charcode);
      }
    }
    charcode = get_next_char(ft_face, charcode, &idx);
  }
}

/*
 * Print usage instructions and default values for styles
 */
static void usage(const char *cmd) {
  const struct fntsample_style *style;

  fprintf(stderr, _("Usage: %s [ OPTIONS ] -f FONT-FILE -o OUTPUT-FILE\n"
      "       %s -h\n\n"), cmd, cmd);
  fprintf(stderr,
      _("Options:\n"
          "  --font-file,         -f FONT-FILE    Create samples of FONT-FILE\n"
          "  --font-index,        -n IDX          Font index in FONT-FILE\n"
          "  --output-file,       -o OUTPUT-FILE  Save samples to OUTPUT-FILE\n"
          "  --help,              -h              Show this information message and exit\n"
          "  --other-font-file,   -d OTHER-FONT   Compare FONT-FILE with OTHER-FONT and highlight added glyphs\n"
          "  --other-index,       -m IDX          Font index in OTHER-FONT\n"
          "  --postscript-output, -s              Use PostScript format for output instead of PDF\n"
          "  --svg,               -g              Use SVG format for output\n"
          "  --print-outline,     -l              Print document outlines data to standard output\n"
          "  --include-range,     -i RANGE        Show characters in RANGE\n"
          "  --exclude-range,     -x RANGE        Do not show characters in RANGE\n"
          "  --ucd-xml-file,      -r XML_FILE     UCD data in XML_FILE\n"
          "  --style,             -t \"STYLE: VAL\" Set STYLE to value VAL\n"));
  fprintf(stderr, _("\nSupported styles (and default values):\n"));
  for (style = styles; style->name; style++)
    fprintf(stderr, "\t%s (%s)\n", style->name, style->default_val);
}

/*
 * Try to get font name for a given font face.
 * Returned name should be free()'d after use.
 * If function cannot allocate memory, it terminates the program.
 */
static const char *get_font_name(FT_Face face) {
  FT_Error error;
  FT_SfntName face_name;
  char *fontname;

  /* try SFNT format */
  error = FT_Get_Sfnt_Name(face, 4 /* full font name */, &face_name);
  if (!error) {
    fontname = malloc(face_name.string_len + 1);
    if (!fontname) {
      perror("malloc");
      exit(1);
    }
    memcpy(fontname, face_name.string, face_name.string_len);
    fontname[face_name.string_len] = '\0';
    return fontname;
  }

  /* try Type1 format */
  PS_FontInfoRec fontinfo;

  error = FT_Get_PS_Font_Info(face, &fontinfo);
  if (!error) {
    if (fontinfo.full_name) {
      fontname = strdup(fontinfo.full_name);
      if (!fontname) {
        perror("strdup");
        exit(1);
      }
      return fontname;
    }
  }

  /* fallback */
  const char *family_name = face->family_name ? face->family_name : "Unknown";
  const char *style_name = face->style_name ? face->style_name : "";
  size_t len = strlen(family_name) + strlen(style_name) + 1/* for space */;

  fontname = malloc(len + 1);
  if (!fontname) {
    perror("malloc");
    exit(1);
  }

  sprintf(fontname, "%s %s", family_name, style_name);
  return fontname;
}

/*
 * Initialize fonts used to print table headers and character codes.
 */
static void init_pango_fonts(void) {
  /* FIXME is this correct? */
  PangoCairoFontMap *map =
      (PangoCairoFontMap *) pango_cairo_font_map_get_default();

  pango_cairo_font_map_set_resolution(map, 72.0);

  header_font = pango_font_description_from_string(get_style("header-font"));
  font_name_font = pango_font_description_from_string(
      get_style("font-name-font"));
  table_numbers_font = pango_font_description_from_string(
      get_style("table-numbers-font"));
  cell_numbers_font = pango_font_description_from_string(
      get_style("cell-numbers-font"));
}

/*
 * Calculate various offsets.
 */
static void calculate_offsets(cairo_t *cr) {
  PangoRectangle extents;
  /* Assume that vertical extents does not depend on actual text */
  PangoLayout *l = layout_text(cr, cell_numbers_font, "0123456789ABCDEF",
      &extents);
  g_object_unref(l);
  /* Unsolved mistery of pango's font metrics.... */
  double digits_ascent = pango_units_to_double(PANGO_DESCENT(extents));
  double digits_descent = -pango_units_to_double(PANGO_ASCENT(extents));

  cell_label_offset = digits_descent + 2;
  cell_glyph_bot_offset = cell_label_offset + digits_ascent + 2;
}

/*
 * Create cairo scaled font with the best size (hopefuly...)
 */
static cairo_scaled_font_t *create_default_font(FT_Face ft_face) {
  cairo_font_face_t *cr_face = cairo_ft_font_face_create_for_ft_face(ft_face,
      0);
  cairo_matrix_t font_matrix;
  cairo_matrix_t ctm;
  cairo_font_options_t *options = cairo_font_options_create();
  cairo_scaled_font_t *cr_font;
  cairo_font_extents_t extents;

  /* First create font with size 1 and measure it */
  cairo_matrix_init_identity(&font_matrix);
  cairo_matrix_init_identity(&ctm);
  /* Turn off rounding, so we can get real metrics */
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  cr_font = cairo_scaled_font_create(cr_face, &font_matrix, &ctm, options);
  cairo_scaled_font_extents(cr_font, &extents);

  /* Use some magic to find the best font size... */
  double tgt_size = cell_height - cell_glyph_bot_offset - 2;
  if (tgt_size <= 0) {
    fprintf(stderr,
        _("Not enough space for rendering glyphs. Make cell font smaller.\n"));
    exit(5);
  }
  double act_size = extents.ascent + extents.descent;
  if (act_size <= 0) {
    fprintf(stderr, _("The font has strange metrics: ascent + descent = %g\n"),
        act_size);
    exit(5);
  }
  double scale = tgt_size / act_size;
  if (scale > 1)
    scale = trunc(scale); // just to make numbers nicer
  if (scale > 20)
    scale = 20; // Do not make font larger than in previous versions

  cairo_scaled_font_destroy(cr_font);

  /* Create the font once again, but this time scaled */
  cairo_matrix_init_scale(&font_matrix, scale, scale);
  cr_font = cairo_scaled_font_create(cr_face, &font_matrix, &ctm, options);
  cairo_scaled_font_extents(cr_font, &extents);
  glyph_baseline_offset = (tgt_size - (extents.ascent + extents.descent)) / 2
      + 2 + extents.ascent;
  return cr_font;
}

int main(int argc, char **argv) {
  cairo_surface_t *surface;
  cairo_t *cr;
  FT_Error error;
  FT_Library library;
  FT_Face face, other_face = NULL;
  const char *fontname; /* full name of the font */
  cairo_status_t cr_status;
  cairo_scaled_font_t *cr_font;

  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE, LOCALEDIR);
  textdomain(PACKAGE);

  parse_options(argc, argv);

  error = FT_Init_FreeType(&library);
  if (error) {
    /* TRANSLATORS: 'freetype' is a name of a library, and should be left untranslated */
    fprintf(stderr, _("%s: freetype error\n"), argv[0]);
    exit(3);
  }

  error = FT_New_Face(library, font_file_name, font_index, &face);
  if (error) {
    fprintf(stderr, _("%s: failed to open font file %s\n"), argv[0],
        font_file_name);
    exit(4);
  }

  fontname = get_font_name(face);

  if (other_font_file_name) {
    error = FT_New_Face(library, other_font_file_name, other_index,
        &other_face);
    if (error) {
      fprintf(stderr, _("%s: failed to create new font face\n"), argv[0]);
      exit(4);
    }
  }

  if (postscript_output)
    surface = cairo_ps_surface_create(output_file_name, A4_WIDTH, A4_HEIGHT);
  else if (svg_output)
    surface = cairo_svg_surface_create(output_file_name, A4_WIDTH, A4_HEIGHT);
  else surface = cairo_pdf_surface_create(output_file_name, A4_WIDTH,
      A4_HEIGHT); /* A4 paper */

  cr_status = cairo_surface_status(surface);
  if (cr_status != CAIRO_STATUS_SUCCESS) {
    /* TRANSLATORS: 'cairo' is a name of a library, and should be left untranslated */
    fprintf(stderr, _("%s: failed to create cairo surface: %s\n"), argv[0],
        cairo_status_to_string(cr_status));
    exit(1);
  }

  cr = cairo_create(surface);
  cr_status = cairo_status(cr);
  if (cr_status != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, _("%s: cairo_create failed: %s\n"), argv[0],
        cairo_status_to_string(cr_status));
    exit(1);
  }

  cairo_surface_destroy(surface);

  init_pango_fonts();
  calculate_offsets(cr);
  cr_font = create_default_font(face);
  cr_status = cairo_scaled_font_status(cr_font);
  if (cr_status != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, _("%s: failed to create scaled font: %s\n"), argv[0],
        cairo_status_to_string(cr_status));
    exit(1);
  }

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  draw_glyphs(cr, cr_font, face, fontname, other_face);
  cairo_destroy(cr);
  return 0;
}
