/* NaOS GOP framebuffer console. Bitmap font (DejaVu Sans Mono, rasterized
 * at build time by tools/font_render.py) blitted from a 2-bit alpha cache.
 * Own blit code; font DATA comes from the installed package (user exception
 * to the zero-prebuilt rule).
 *
 * Text model: every printed char is recorded in a ring of lines (with the
 * colors in force), so PgUp/PgDn can scroll back after the pixels scrolled
 * away. Live output draws incrementally (fast path); scroll keys, font
 * switches and clears repaint from the ring. Font scale 2 doubles pixels. */
#include "fb.h"
#include "font_bitmap.h"   /* generated: build/font_bitmap.h via -Ibuild */
#include "../../common/cstring.h"

#define GLYPH_W 12u
#define GLYPH_H 24u

#define CON_LINES 256      /* ring: everything stays until `clear` */
#define CON_COLS 256       /* widest line kept (paints truncated to screen) */

_Static_assert(FONT_CELL_W == 12 && FONT_CELL_H == 24,
               "font_bitmap.h cell must be 12x24");

static uint8_t *g_base;
static uint8_t g_cache[128][24][3]; /* 2-bit alpha per px: 12px = 3B/row */
static int g_font_ready;
static uint32_t g_w, g_h, g_pitch;
static uint32_t g_fg = 0xFFFFFF, g_bg = 0x000000;
static uint32_t g_cx, g_cy;
static int g_fmt;   /* 0 RGBX, 1 BGRX */
static int g_ok;
static int g_scale = 1; /* 1 normal, 2 big */

/* Text ring. */
static char con_text[CON_LINES][CON_COLS];
static uint16_t con_len[CON_LINES];
static uint32_t con_fg[CON_LINES], con_bg[CON_LINES];
static uint32_t con_total; /* committed lines ever */
static char con_cur[CON_COLS];
static uint32_t con_cur_len, con_pos;
static uint32_t con_view;   /* lines back from live (0 = live) */
static uint32_t con_ftotal; /* frozen total while scrolled */
static int cursor_on;       /* vertical bar currently drawn */

/* The bitmap header layout matches g_cache exactly; just copy it in. */
static void font_load(void) {
  _Static_assert(sizeof(g_cache) == sizeof(kFontBits), "font cache mismatch");
  naos_memcpy(g_cache, kFontBits, sizeof(g_cache));
}

void fb_init(uint64_t base, uint32_t width, uint32_t height, uint32_t pitch,
             uint32_t bpp, uint32_t format) {
  g_ok = 0;
  if (base == 0 || width == 0 || height == 0 || bpp != 32 || pitch == 0) return;
  g_base = (uint8_t *)(uintptr_t)base;
  g_w = width; g_h = height; g_pitch = pitch;
  g_fmt = (format == 1) ? 1 : 0;
  g_cx = 0; g_cy = 0;
  if (!g_font_ready) {
    font_load();
    g_font_ready = 1;
  }
  g_ok = 1;
}

int fb_ready(void) { return g_ok; }
int fb_font(void) { return g_scale; }
int fb_is_live(void) { return !con_view; }

static uint32_t cell_w(void) { return GLYPH_W * (uint32_t)g_scale; }
static uint32_t cell_h(void) { return GLYPH_H * (uint32_t)g_scale; }

static uint32_t ncols(void) {
  uint32_t c = g_w / cell_w();
  if (c > CON_COLS) c = CON_COLS;
  return c ? c : 1;
}

static uint32_t nrows(void) {
  uint32_t r = g_h / cell_h();
  return r ? r : 1;
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
  if (!g_ok || x >= g_w || y >= g_h) return;
  uint8_t *p = g_base + (uint64_t)y * g_pitch + (uint64_t)x * 4u;
  uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
  if (g_fmt == 0) { p[0] = r; p[1] = g; p[2] = b; }
  else            { p[0] = b; p[1] = g; p[2] = r; }
  p[3] = 0;
}

static void paint_bg(void) {
  for (uint32_t y = 0; y < g_h; y++)
    for (uint32_t x = 0; x < g_w; x++)
      put_pixel(x, y, g_bg);
}

static void repaint(void); /* defined below draw_glyph */

void fb_clear(uint32_t rgb) {
  if (!g_ok) return;
  g_bg = rgb;
  con_total = 0;
  con_cur_len = 0;
  con_pos = 0;
  con_view = 0;
  cursor_on = 0;
  paint_bg();
  g_cx = 0; g_cy = 0;
}

void fb_set_colors(uint32_t fg, uint32_t bg) {
  g_fg = fg;
  g_bg = bg;
}

static void con_commit(void) {
  uint32_t i = con_total % CON_LINES;
  uint32_t n = con_cur_len > CON_COLS ? CON_COLS : con_cur_len;
  naos_memcpy(con_text[i], con_cur, n);
  con_len[i] = (uint16_t)n;
  con_fg[i] = g_fg;
  con_bg[i] = g_bg;
  con_total++;
  con_cur_len = 0;
  con_pos = 0;
  /* Scrolled far behind a flood of output: the frozen window would be
   * overwritten in the ring, so rejoin live instead of showing garbage. */
  if (con_view && con_total - con_ftotal >= CON_LINES) {
    con_view = 0;
    repaint();
  }
}

static void scroll(void) {
  uint32_t rows = nrows();
  if (g_cy < rows) return;
  /* Move screen up by one glyph row using 32-bit copies. */
  uint32_t row_bytes = cell_h() * g_pitch;
  uint8_t *dst = g_base;
  uint8_t *src = g_base + row_bytes;
  uint64_t n = (uint64_t)(rows - 1) * row_bytes;
  naos_memmove(dst, src, (size_t)n);
  /* Clear last row. */
  uint8_t *last = g_base + (uint64_t)(rows - 1) * row_bytes;
  for (uint64_t i = 0; i < row_bytes; i += 4) {
    uint8_t r = (uint8_t)(g_bg >> 16), g = (uint8_t)(g_bg >> 8),
            b = (uint8_t)g_bg;
    if (g_fmt == 0) { last[i] = r; last[i+1] = g; last[i+2] = b; }
    else            { last[i] = b; last[i+1] = g; last[i+2] = r; }
    last[i+3] = 0;
  }
  g_cy = rows - 1;
}

static void draw_glyph(char c, uint32_t cx, uint32_t cy, uint32_t fg,
                       uint32_t bg) {
  uint8_t ch = ((uint8_t)c < 128) ? (uint8_t)c : (uint8_t)'?';
  uint8_t fr = (uint8_t)(fg >> 16), fg2 = (uint8_t)(fg >> 8),
          fb2 = (uint8_t)fg;
  uint8_t br = (uint8_t)(bg >> 16), bg2 = (uint8_t)(bg >> 8),
          bb2 = (uint8_t)bg;
  uint32_t sc = (uint32_t)g_scale;
  for (uint32_t r = 0; r < GLYPH_H; r++) {
    for (uint32_t col = 0; col < GLYPH_W; col++) {
      uint32_t a = (g_cache[ch][r][col / 4] >> (6 - (col % 4) * 2)) & 3u;
      uint8_t o0 = (uint8_t)(br + ((uint32_t)(fr - br) * a + 1u) / 3u);
      uint8_t o1 = (uint8_t)(bg2 + ((uint32_t)(fg2 - bg2) * a + 1u) / 3u);
      uint8_t o2 = (uint8_t)(bb2 + ((uint32_t)(fb2 - bb2) * a + 1u) / 3u);
      for (uint32_t dy = 0; dy < sc; dy++) {
        for (uint32_t dx = 0; dx < sc; dx++) {
          uint32_t xx = cx * cell_w() + col * sc + dx;
          uint32_t yy = cy * cell_h() + r * sc + dy;
          if (xx >= g_w || yy >= g_h) continue;
          uint8_t *p = g_base + (uint64_t)yy * g_pitch + (uint64_t)xx * 4u;
          if (g_fmt == 0) { p[0] = o0; p[1] = o1; p[2] = o2; }
          else            { p[0] = o2; p[1] = o1; p[2] = o0; }
          p[3] = 0;
        }
      }
    }
  }
}

/* Paint one stored line at a screen row. */
static void paint_line(uint32_t idx, uint32_t srow, uint32_t cols) {
  uint32_t n = con_len[idx];
  if (n > cols) n = cols;
  for (uint32_t k = 0; k < n; k++)
    draw_glyph(con_text[idx][k], k, srow, con_fg[idx], con_bg[idx]);
}

/* Vertical bar cursor at the live input position. */
static void cursor_rect(uint32_t rgb) {
  uint32_t w = (g_scale == 2) ? 4u : 2u;
  uint32_t h = cell_h();
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++)
      put_pixel(g_cx * cell_w() + x, g_cy * cell_h() + y, rgb);
}

void fb_cursor_show(void) {
  if (!g_ok || cursor_on) return;
  cursor_rect(g_fg);
  cursor_on = 1;
}

void fb_cursor_hide(void) {
  if (!g_ok || !cursor_on) return;
  cursor_rect(g_bg);
  cursor_on = 0;
}

static void repaint(void) {
  uint32_t rows = nrows(), cols = ncols();
  /* Frozen view ends `view` lines back; live ends at the newest line.
   * (maxback guarantees view <= base - rows, so no underflow.) */
  uint32_t base = con_view ? con_ftotal : con_total;
  uint32_t end = con_view ? (base - con_view) : base;
  uint32_t first, count;
  cursor_on = 0; /* paint_bg wipes it */
  paint_bg();
  if (!con_view) {
    /* Live: last rows-1 committed lines, input line on the bottom row. */
    uint32_t keep = (rows > 0) ? rows - 1 : 0;
    first = (end > keep) ? (end - keep) : 0;
    count = end - first;
    for (uint32_t i = 0; i < count; i++)
      paint_line((first + i) % CON_LINES, keep - count + i, cols);
    {
      uint32_t m = con_cur_len > cols ? cols : con_cur_len;
      for (uint32_t k = 0; k < m; k++)
        draw_glyph(con_cur[k], k, rows - 1, g_fg, g_bg);
    }
    g_cx = con_pos > cols ? cols : con_pos;
    g_cy = rows - 1;
  } else {
    /* Frozen: rows of history ending `view` lines back. */
    first = (end > rows) ? (end - rows) : 0;
    count = end - first;
    for (uint32_t i = 0; i < count; i++)
      paint_line((first + i) % CON_LINES, rows - count + i, cols);
    g_cx = 0;
    g_cy = rows - 1;
  }
}

void fb_scroll_back(void) {
  uint32_t rows, maxback, step, base;
  if (!g_ok) return;
  rows = nrows();
  step = (rows > 1) ? (rows - 1) : 1;
  /* While already scrolled, measure from the frozen total so the view
   * can never run past the oldest frozen line (uint32 underflow). */
  base = con_view ? con_ftotal : con_total;
  maxback = (base > rows) ? (base - rows) : 0;
  if (maxback == 0) return;
  if (!con_view) con_ftotal = con_total;
  con_view += step;
  if (con_view > maxback) con_view = maxback;
  repaint();
}

void fb_scroll_fwd(void) {
  uint32_t rows, step;
  if (!g_ok || !con_view) return;
  rows = nrows();
  step = (rows > 1) ? (rows - 1) : 1;
  if (con_view <= step) con_view = 0;
  else con_view -= step;
  repaint();
}

void fb_scroll_live(void) {
  if (!g_ok || !con_view) return;
  con_view = 0;
  repaint();
}

void fb_set_font(int scale) {
  if (scale != 1 && scale != 2) return;
  if (!g_ok || scale == g_scale) return;
  g_scale = scale;
  con_view = 0;
  repaint();
}

void fb_putc(char c) {
  uint32_t cols;
  if (!g_ok) return;
  fb_cursor_hide(); /* output always wins over the bar */
  cols = ncols();
  if (c == '\n') {
    con_commit();
    if (!con_view) {
      g_cx = 0;
      g_cy++;
      scroll();
    }
    return;
  }
  if (c == '\r') {
    con_pos = 0;
    if (!con_view) g_cx = 0;
    return;
  }
  if (con_pos >= cols || con_pos >= CON_COLS) {
    con_commit();
    if (!con_view) {
      g_cx = 0;
      g_cy++;
      scroll();
    }
  }
  if (con_pos < CON_COLS) {
    con_cur[con_pos] = c;
    if (con_pos >= con_cur_len) con_cur_len = con_pos + 1;
  }
  con_pos++;
  if (!con_view) {
    scroll();
    draw_glyph(c, g_cx, g_cy, g_fg, g_bg);
    g_cx++;
  }
}

void fb_puts(const char *s) {
  while (*s) fb_putc(*s++);
}

void fb_backspace(void) {
  uint32_t cols;
  if (!g_ok) return;
  fb_cursor_hide();
  cols = ncols();
  if (con_pos == 0) return;
  con_pos--;
  if (con_cur_len > 0) con_cur_len--;
  if (!con_view) {
    if (g_cx == 0) return;
    g_cx--;
    if (g_cx >= cols) g_cx = cols - 1;
    draw_glyph(' ', g_cx, g_cy, g_fg, g_bg);
  }
}

void fb_put_hex64(uint64_t v) {
  static const char kHex[] = "0123456789ABCDEF";
  fb_puts("0x");
  for (int i = 0; i < 16; i++) fb_putc(kHex[(v >> (60 - 4 * i)) & 0xF]);
}

void fb_put_uint64(uint64_t v) {
  char tmp[21];
  int n = 0;
  if (v == 0) { fb_putc('0'); return; }
  while (v > 0 && n < 20) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n > 0) fb_putc(tmp[--n]);
}
