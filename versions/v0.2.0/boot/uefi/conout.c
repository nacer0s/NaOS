/* NaOS bootloader ConOut helpers. */
#include "conout.h"
#include "../../common/cstring.h"

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con;

void co_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con) {
  g_con = con;
}

static void emit(const CHAR16 *s) {
  if (g_con && g_con->OutputString) g_con->OutputString(g_con, s);
}

void co_puts(const char *ascii) {
  /* Convert ASCII to UCS-2 in chunks (ConOut needs CHAR16 + \r\n newlines). */
  CHAR16 buf[128];
  size_t i = 0;
  while (*ascii) {
    char c = *ascii++;
    if (c == '\n') {
      buf[i++] = (CHAR16)'\r';
      buf[i++] = (CHAR16)'\n';
    } else {
      buf[i++] = (CHAR16)c;
    }
    if (i >= 125) { buf[i] = 0; emit(buf); i = 0; }
  }
  buf[i] = 0;
  emit(buf);
}

void co_put_hex64(uint64_t v) {
  static const char kHex[] = "0123456789ABCDEF";
  char tmp[19];
  tmp[0] = '0'; tmp[1] = 'x';
  for (int i = 0; i < 16; i++) tmp[2 + i] = kHex[(v >> (60 - 4 * i)) & 0xF];
  tmp[18] = '\0';
  co_puts(tmp);
}

void co_put_uint64(uint64_t v) {
  char tmp[21];
  int n = 0;
  if (v == 0) { co_puts("0"); return; }
  while (v > 0 && n < 20) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  /* reverse in place */
  for (int i = 0; i < n / 2; i++) {
    char t = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = t;
  }
  tmp[n] = '\0';
  co_puts(tmp);
}
