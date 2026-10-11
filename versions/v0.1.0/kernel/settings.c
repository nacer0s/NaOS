/* NaOS settings. /SETTINGS.TXT holds `key=value` lines (`#` comments).
 * Known keys: prompt (word), fg / bg (RRGGBB hex). Missing file or bad
 * lines keep built-in defaults. Own code. */
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
static int g_from_file; /* 1 once a readable file supplied any key */

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
}

void settings_show(void) {
  kprintf("prompt=%s fg=%x bg=%x (%s)\n", g_prompt, (unsigned)g_fg,
          (unsigned)g_bg, g_from_file ? "/SETTINGS.TXT" : "defaults");
}

const char *settings_prompt(void) { return g_prompt; }
uint32_t settings_fg(void) { return g_fg; }
uint32_t settings_bg(void) { return g_bg; }
