/* NaOS PS/2 keyboard driver. Set 1, interrupt-driven ring buffer. */
#include "ps2_kbd.h"
#include "../../common/io.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBUF_SIZE 256

static volatile uint8_t g_buf[KBUF_SIZE];
static volatile uint32_t g_head, g_tail;
static int g_shift, g_caps, g_ext;

void kbd_init(void) {
  g_head = g_tail = 0;
  g_shift = g_caps = g_ext = 0;
  /* Drain stale controller output. */
  for (int i = 0; i < 64; i++) {
    if ((naos_inb(KBD_STAT) & 0x01) == 0) break;
    (void)naos_inb(KBD_DATA);
  }
}

void kbd_push(uint8_t sc) {
  uint32_t next = (g_head + 1) % KBUF_SIZE;
  if (next == g_tail) return; /* full: drop */
  g_buf[g_head] = sc;
  g_head = next;
}

int kbd_empty(void) {
  return g_head == g_tail;
}

static char map_digit(uint8_t row, int shift) {
  static const char kPlain[] = "1234567890";
  static const char kShift[] = "!@#$%^&*()";
  if (row < 10) return shift ? kShift[row] : kPlain[row];
  return 0;
}

/* Translate one scancode (make only) to ASCII. Returns 0 for modifiers/none. */
static char translate(uint8_t sc) {
  /* Letters: base lowercase, upper if shift XOR caps. */
  switch (sc) {
    case 0x10: return (g_shift ^ g_caps) ? 'Q' : 'q';
    case 0x11: return (g_shift ^ g_caps) ? 'W' : 'w';
    case 0x12: return (g_shift ^ g_caps) ? 'E' : 'e';
    case 0x13: return (g_shift ^ g_caps) ? 'R' : 'r';
    case 0x14: return (g_shift ^ g_caps) ? 'T' : 't';
    case 0x15: return (g_shift ^ g_caps) ? 'Y' : 'y';
    case 0x16: return (g_shift ^ g_caps) ? 'U' : 'u';
    case 0x17: return (g_shift ^ g_caps) ? 'I' : 'i';
    case 0x18: return (g_shift ^ g_caps) ? 'O' : 'o';
    case 0x19: return (g_shift ^ g_caps) ? 'P' : 'p';
    case 0x1E: return (g_shift ^ g_caps) ? 'A' : 'a';
    case 0x1F: return (g_shift ^ g_caps) ? 'S' : 's';
    case 0x20: return (g_shift ^ g_caps) ? 'D' : 'd';
    case 0x21: return (g_shift ^ g_caps) ? 'F' : 'f';
    case 0x22: return (g_shift ^ g_caps) ? 'G' : 'g';
    case 0x23: return (g_shift ^ g_caps) ? 'H' : 'h';
    case 0x24: return (g_shift ^ g_caps) ? 'J' : 'j';
    case 0x25: return (g_shift ^ g_caps) ? 'K' : 'k';
    case 0x26: return (g_shift ^ g_caps) ? 'L' : 'l';
    case 0x2C: return (g_shift ^ g_caps) ? 'Z' : 'z';
    case 0x2D: return (g_shift ^ g_caps) ? 'X' : 'x';
    case 0x2E: return (g_shift ^ g_caps) ? 'C' : 'c';
    case 0x2F: return (g_shift ^ g_caps) ? 'V' : 'v';
    case 0x30: return (g_shift ^ g_caps) ? 'B' : 'b';
    case 0x31: return (g_shift ^ g_caps) ? 'N' : 'n';
    case 0x32: return (g_shift ^ g_caps) ? 'M' : 'm';
    default: break;
  }
  if (sc >= 0x02 && sc <= 0x0B) {
    uint8_t row = (uint8_t)(sc - 0x02); /* 0..9 -> "1234567890" */
    return map_digit(row, g_shift);
  }
  switch (sc) {
    case 0x0C: return g_shift ? '_' : '-';
    case 0x0D: return g_shift ? '+' : '=';
    case 0x0E: return '\b';
    case 0x0F: return '\t';
    case 0x1A: return g_shift ? '{' : '[';
    case 0x1B: return g_shift ? '}' : ']';
    case 0x1C: return '\n';
    case 0x27: return g_shift ? ':' : ';';
    case 0x28: return g_shift ? '"' : '\'';
    case 0x29: return g_shift ? '~' : '`';
    case 0x2B: return g_shift ? '|' : '\\';
    case 0x33: return g_shift ? '<' : ',';
    case 0x34: return g_shift ? '>' : '.';
    case 0x35: return g_shift ? '?' : '/';
    case 0x39: return ' ';
    default: return 0;
  }
}

/* Extended (0xE0) make codes we care about; others ignored. */
static int translate_ext(uint8_t sc) {
  switch (sc) {
    case 0x48: return KBD_UP;
    case 0x50: return KBD_DOWN;
    case 0x4B: return KBD_LEFT;
    case 0x4D: return KBD_RIGHT;
    default: return 0;
  }
}

int kbd_getc(void) {
  for (;;) {
    /* Wait for a scancode with interrupts on. */
    while (kbd_empty()) { __asm__ volatile ("hlt"); }
    __asm__ volatile ("cli");
    uint8_t sc = 0;
    int have = 0;
    if (!kbd_empty()) {
      sc = g_buf[g_tail];
      g_tail = (g_tail + 1) % KBUF_SIZE;
      have = 1;
    }
    __asm__ volatile ("sti");
    if (!have) continue;

    if (sc == 0xE0) { g_ext = 1; continue; } /* extended: prefix for next */
    if (g_ext) {
      g_ext = 0;
      if (sc & 0x80) continue; /* extended release: ignore */
      int code = translate_ext(sc);
      if (code) return code;
      continue;
    }
    if (sc & 0x80) { /* release */
      uint8_t code = (uint8_t)(sc & 0x7F);
      if (code == 0x2A || code == 0x36) g_shift = 0;
      continue;
    }
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; continue; }
    if (sc == 0x3A) { g_caps = !g_caps; continue; }
    char c = translate(sc);
    if (c) return c;
    /* non-printable make code: ignore */
  }
}
