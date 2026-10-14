/* NaOS GOP framebuffer text console. 32bpp, RGBX/BGRX aware,
 * DejaVu 12x24 / 8x16 cells, scrollback, repaint-only scrolling. */
#ifndef NAOS_FB_H
#define NAOS_FB_H

#include <stdint.h>

void fb_init(uint64_t base, uint32_t width, uint32_t height, uint32_t pitch,
             uint32_t bpp, uint32_t format);
int fb_ready(void);
void fb_clear(uint32_t rgb);
void fb_set_colors(uint32_t fg, uint32_t bg);
void fb_putc(char c);
void fb_puts(const char *s);
void fb_backspace(void);
void fb_put_hex64(uint64_t v);
void fb_put_uint64(uint64_t v);
/* Scrollback (PgUp/PgDn): text ring keeps 256 lines; view freezes while
 * you read, any shell key jumps back to live. */
void fb_scroll_back(void); /* one screen up */
void fb_scroll_fwd(void);  /* one screen down */
void fb_scroll_live(void); /* jump to live tail */
int fb_is_live(void);
/* Font size: 0 = small 8x16, 1 = normal 12x24, 2 = big, 3-4 = larger.
 * Repaints. */
void fb_set_font(int scale);
int fb_font(void);
/* Blinking input cursor (vertical bar). Stateless by design: there is no
 * visibility flag and no show/hide — caret_refresh() repaints the caret
 * cell from the text ring (input char or blank) plus the bar when the
 * blink phase says so. Every paint derives from ring state, so a stale
 * or double bar is impossible. Call it after any caret move, draw,
 * clear, or repaint; it no-ops off the live tail. Safe from IRQ context
 * (cell stores only; races with task output heal next refresh). */
void caret_refresh(void);

#endif /* NAOS_FB_H */
