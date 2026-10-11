/* NaOS GOP framebuffer console. Bitmap font (DejaVu Sans Mono, rasterized
 * at build time by tools/font_render.py) blitted from a 2-bit alpha cache.
 * Own blit code; font DATA comes from the installed package (user exception
 * to the zero-prebuilt rule). */
#include "fb.h"
#include "font_bitmap.h"   /* generated: build/font_bitmap.h via -Ibuild */
#include "../../common/cstring.h"

#define GLYPH_W 12u
#define GLYPH_H 24u

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

static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
  if (!g_ok || x >= g_w || y >= g_h) return;
  uint8_t *p = g_base + (uint64_t)y * g_pitch + (uint64_t)x * 4u;
  uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
  if (g_fmt == 0) { p[0] = r; p[1] = g; p[2] = b; }
  else            { p[0] = b; p[1] = g; p[2] = r; }
  p[3] = 0;
}

void fb_clear(uint32_t rgb) {
  if (!g_ok) return;
  for (uint32_t y = 0; y < g_h; y++)
    for (uint32_t x = 0; x < g_w; x++)
      put_pixel(x, y, rgb);
  g_cx = 0; g_cy = 0;
}

void fb_set_colors(uint32_t fg, uint32_t bg) {
  g_fg = fg;
  g_bg = bg;
}

static void scroll(void) {
  uint32_t cols = g_w / GLYPH_W;
  uint32_t rows = g_h / GLYPH_H;
  (void)cols;
  if (g_cy < rows) return;
  /* Move screen up by one glyph row using 32-bit copies. */
  uint32_t row_bytes = GLYPH_H * g_pitch;
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

static void draw_glyph(char c, uint32_t cx, uint32_t cy) {
  uint8_t ch = ((uint8_t)c < 128) ? (uint8_t)c : (uint8_t)'?';
  uint8_t fr = (uint8_t)(g_fg >> 16), fg2 = (uint8_t)(g_fg >> 8),
          fb2 = (uint8_t)g_fg;
  uint8_t br = (uint8_t)(g_bg >> 16), bg2 = (uint8_t)(g_bg >> 8),
          bb2 = (uint8_t)g_bg;
  for (uint32_t r = 0; r < GLYPH_H; r++) {
    for (uint32_t col = 0; col < GLYPH_W; col++) {
      uint32_t xx = cx * GLYPH_W + col, yy = cy * GLYPH_H + r;
      if (xx >= g_w || yy >= g_h) continue;
      uint32_t a = (g_cache[ch][r][col / 4] >> (6 - (col % 4) * 2)) & 3u;
      uint8_t *p = g_base + (uint64_t)yy * g_pitch + (uint64_t)xx * 4u;
      uint8_t o0 = (uint8_t)(br + ((uint32_t)(fr - br) * a + 1u) / 3u);
      uint8_t o1 = (uint8_t)(bg2 + ((uint32_t)(fg2 - bg2) * a + 1u) / 3u);
      uint8_t o2 = (uint8_t)(bb2 + ((uint32_t)(fb2 - bb2) * a + 1u) / 3u);
      if (g_fmt == 0) { p[0] = o0; p[1] = o1; p[2] = o2; }
      else            { p[0] = o2; p[1] = o1; p[2] = o0; }
      p[3] = 0;
    }
  }
}

void fb_putc(char c) {
  if (!g_ok) return;
  uint32_t cols = g_w / GLYPH_W;
  if (c == '\n') { g_cx = 0; g_cy++; scroll(); return; }
  if (c == '\r') { g_cx = 0; return; }
  if (g_cx >= cols) { g_cx = 0; g_cy++; }
  scroll();
  draw_glyph(c, g_cx, g_cy);
  g_cx++;
}

void fb_puts(const char *s) {
  while (*s) fb_putc(*s++);
}

void fb_backspace(void) {
  if (!g_ok) return;
  uint32_t cols = g_w / GLYPH_W;
  if (g_cx == 0) return;
  g_cx--;
  if (g_cx >= cols) g_cx = cols - 1;
  draw_glyph(' ', g_cx, g_cy);
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
