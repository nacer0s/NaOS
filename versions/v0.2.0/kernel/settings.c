/* NaOS settings. /SETTINGS.TXT holds `key=value` lines (`#` comments).
 * Known keys: prompt (word), fg / bg (RRGGBB hex), font (normal/big).
 * Missing file or bad lines keep built-in defaults. Own code. */
#include "settings.h"
#include "fs/fat32.h"
#include "drv/fb.h"
#include "lib/kprintf.h"
#include "../common/cstring.h"

#define PROMPT_MAX 31
#define FILE_MAX 1024

static char g_prompt[PROMPT_MAX + 1] = "NaOS";
static uint32_t g_fg = 0xFFFFFF;
static uint32_t g_bg = 0x000000;
static int g_font = 1; /* 0 small, 1 normal, 2 big, 3 3x, 4 4x */
static int g_from_file; /* 1 once a readable file supplied any key */

/* Font words <-> ids. */
static const char *font_word(int f) {
  if (f == 0) return "small";
  if (f == 1) return "normal";
  if (f == 2) return "big";
  if (f == 3) return "3";
  return "4";
}

static int font_val(const char *s) {
  if (naos_strcmp(s, "small") == 0) return 0;
  if (naos_strcmp(s, "normal") == 0 || naos_strcmp(s, "1") == 0) return 1;
  if (naos_strcmp(s, "big") == 0 || naos_strcmp(s, "2") == 0) return 2;
  if (naos_strcmp(s, "3") == 0) return 3;
  if (naos_strcmp(s, "4") == 0) return 4;
  return -1;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parse exactly 6 hex digits into *out. 0 ok. */
static int parse_rgb(const char *s, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    int h = hex_val(s[i]);
    if (h < 0) return -1;
    v = (v << 4) | (uint32_t)h;
  }
  if (s[6] != '\0') return -1;
  *out = v;
  return 0;
}

/* Handle one line (NUL-terminated, no newline). */
static void apply_line(char *line) {
  char *eq;
  if (line[0] == '#' || line[0] == '\0') return;
  eq = 0;
  for (char *p = line; *p; p++) {
    if (*p == '=') { eq = p; break; }
  }
  if (!eq) return;
  *eq = '\0';
  {
    char *val = eq + 1;
    /* Trim trailing spaces/tabs/CR on key and value. */
    char *ke = eq - 1;
    while (ke >= line &&
           (*ke == ' ' || *ke == '\t' || *ke == '\r')) {
      *ke = '\0';
      ke--;
    }
    while (*val == ' ' || *val == '\t') val++;
    {
      char *ve = val;
      while (*ve) ve++;
      while (ve > val &&
             (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) {
        ve--;
        *ve = '\0';
      }
    }
    if (naos_strcmp(line, "prompt") == 0) {
      size_t i = 0;
      while (val[i] && i < PROMPT_MAX) {
        g_prompt[i] = val[i];
        i++;
      }
      g_prompt[i] = '\0';
      if (i > 0) g_from_file = 1;
    } else if (naos_strcmp(line, "fg") == 0) {
      uint32_t v;
      if (parse_rgb(val, &v) == 0) { g_fg = v; g_from_file = 1; }
    } else if (naos_strcmp(line, "bg") == 0) {
      uint32_t v;
      if (parse_rgb(val, &v) == 0) { g_bg = v; g_from_file = 1; }
    } else if (naos_strcmp(line, "font") == 0) {
      int f = font_val(val);
      if (f >= 0) {
        g_font = f;
        g_from_file = 1;
      }
    }
    /* Unknown keys are ignored (forward compatibility). */
  }
}

void settings_load(void) {
  static char buf[FILE_MAX + 1];
  uint32_t size = 0;
  if (fat_read("/SETTINGS.TXT", buf, FILE_MAX, &size) < 0) return;
  buf[size] = '\0';
  {
    char *p = buf;
    while (*p) {
      char *nl = p;
      while (*nl && *nl != '\n') nl++;
      if (*nl == '\n') {
        *nl = '\0';
        apply_line(p);
        p = nl + 1;
      } else {
        apply_line(p);
        break;
      }
    }
  }
  fb_set_colors(g_fg, g_bg);
  fb_set_font(g_font);
}

static void put_hex6(char *out, uint32_t v) {
  static const char kH[] = "0123456789ABCDEF";
  for (int i = 0; i < 6; i++) out[i] = kH[(v >> (20 - 4 * i)) & 0xF];
}

/* Rewrite the whole file from memory (fixed key order). 0 ok. */
int settings_save(void) {
  static char buf[256];
  uint32_t n = 0;
  const char *fontword = font_word(g_font);
  size_t i = 0;
  const char *p1 = "prompt=";
  while (*p1) buf[n++] = *p1++;
  while (g_prompt[i] && n + 32 < sizeof(buf)) buf[n++] = g_prompt[i++];
  buf[n++] = '\n';
  {
    const char *p2 = "fg=";
    while (*p2) buf[n++] = *p2++;
  }
  put_hex6(buf + n, g_fg);
  n += 6;
  buf[n++] = '\n';
  {
    const char *p3 = "bg=";
    while (*p3) buf[n++] = *p3++;
  }
  put_hex6(buf + n, g_bg);
  n += 6;
  buf[n++] = '\n';
  {
    const char *p4 = "font=";
    while (*p4) buf[n++] = *p4++;
  }
  while (*fontword && n + 2 < sizeof(buf)) buf[n++] = *fontword++;
  buf[n++] = '\n';
  if (fat_write("/SETTINGS.TXT", buf, n) != 0) return -1;
  g_from_file = 1;
  return 0;
}

void settings_show(void) {
  kprintf("prompt=%s fg=%x bg=%x font=%s (%s)\n", g_prompt, (unsigned)g_fg,
          (unsigned)g_bg, font_word(g_font),
          g_from_file ? "/SETTINGS.TXT" : "defaults");
}

void settings_set_font(int f) {
  if (f < 0 || f > 4) f = 1;
  g_font = f;
  fb_set_font(g_font);
}

int settings_font(void) { return g_font; }

const char *settings_prompt(void) { return g_prompt; }
uint32_t settings_fg(void) { return g_fg; }
uint32_t settings_bg(void) { return g_bg; }
