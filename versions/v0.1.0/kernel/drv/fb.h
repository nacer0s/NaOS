/* NaOS GOP framebuffer text console. 32bpp, RGBX/BGRX aware, 8x8 font, scroll. */
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
/* Scrollback (PgUp/PgDn): text ring keeps ~128 lines; view freezes while
 * you read, any shell key jumps back to live. */
void fb_scroll_back(void); /* one screen up */
void fb_scroll_fwd(void);  /* one screen down */
void fb_scroll_live(void); /* jump to live tail */
int fb_is_live(void);
/* Font size: 1 = normal 12x24, 2 = big 24x48. Repaints. */
void fb_set_font(int scale);
int fb_font(void);
/* Blinking input cursor (vertical bar). The shell drives show/hide on an
 * idle timer; any output hides it first. Pixels only, never in the ring. */
void fb_cursor_show(void);
void fb_cursor_hide(void);

#endif /* NAOS_FB_H */
