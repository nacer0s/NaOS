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

#endif /* NAOS_FB_H */
