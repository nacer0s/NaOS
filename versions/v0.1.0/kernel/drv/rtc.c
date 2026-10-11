/* NaOS CMOS RTC reader. Handles BCD/binary + 12/24h modes from status B. */
#include "rtc.h"
#include "../../common/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos(uint8_t reg) {
  naos_outb(CMOS_ADDR, (uint8_t)(0x80 | reg)); /* bit 7: NMI off */
  return naos_inb(CMOS_DATA);
}

static uint8_t bcd(uint8_t v) {
  return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

int rtc_read(rtc_time_t *out) {
  /* Wait for update-in-progress to clear (up to ~10ms worst case). */
  for (int i = 0; i < 100000; i++) {
    if ((cmos(0x0A) & 0x80) == 0) break;
  }
  if (cmos(0x0A) & 0x80) return 0;
  uint8_t sec = cmos(0x00);
  uint8_t min = cmos(0x02);
  uint8_t hr = cmos(0x04);
  uint8_t day = cmos(0x07);
  uint8_t mon = cmos(0x08);
  uint8_t yr = cmos(0x09);
  uint8_t regb = cmos(0x0B);
  /* Re-read seconds: rolled over mid-read? */
  if (sec != cmos(0x00)) return rtc_read(out) == 1 ? 1 : 0;

  int binary = (regb & 0x04) != 0;
  int h24 = (regb & 0x02) != 0;
  if (!binary) {
    sec = bcd(sec); min = bcd(min); day = bcd(day); mon = bcd(mon); yr = bcd(yr);
    uint8_t h12 = bcd((uint8_t)(hr & 0x7F));
    if (h24) {
      hr = h12;
    } else {
      int pm = (hr & 0x80) != 0;
      if (h12 == 12) h12 = 0;
      hr = (uint8_t)(h12 + (pm ? 12 : 0));
    }
  } else if (!h24) {
    int pm = (hr & 0x80) != 0;
    uint8_t h12 = (uint8_t)(hr & 0x7F);
    if (h12 == 12) h12 = 0;
    hr = (uint8_t)(h12 + (pm ? 12 : 0));
  }
  if (mon < 1 || mon > 12 || day < 1 || day > 31 || hr > 23 ||
      min > 59 || sec > 59)
    return 0;
  out->year = (uint16_t)(2000 + yr); /* CMOS holds 2-digit year */
  out->month = mon;
  out->day = day;
  out->hour = hr;
  out->minute = min;
  out->second = sec;
  return 1;
}

static void put2(char *b, int v) {
  b[0] = (char)('0' + (v / 10) % 10);
  b[1] = (char)('0' + v % 10);
}

void rtc_format_date(const rtc_time_t *t, char *buf, int bufsz) {
  if (bufsz < 11) { if (bufsz > 0) buf[0] = '\0'; return; }
  buf[0] = (char)('0' + (t->year / 1000) % 10);
  buf[1] = (char)('0' + (t->year / 100) % 10);
  buf[2] = (char)('0' + (t->year / 10) % 10);
  buf[3] = (char)('0' + t->year % 10);
  buf[4] = '-';
  put2(buf + 5, t->month);
  buf[7] = '-';
  put2(buf + 8, t->day);
  buf[10] = '\0';
}

void rtc_format_time(const rtc_time_t *t, char *buf, int bufsz) {
  if (bufsz < 9) { if (bufsz > 0) buf[0] = '\0'; return; }
  put2(buf, t->hour);
  buf[2] = ':';
  put2(buf + 3, t->minute);
  buf[5] = ':';
  put2(buf + 6, t->second);
  buf[8] = '\0';
}
