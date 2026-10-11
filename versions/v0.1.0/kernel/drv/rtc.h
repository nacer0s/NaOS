/* NaOS CMOS RTC: date/time reads (kernel only; user I/O is denied). */
#ifndef NAOS_RTC_H
#define NAOS_RTC_H

#include <stdint.h>

typedef struct {
  uint16_t year;   /* full year, e.g. 2026 */
  uint8_t month;   /* 1-12 */
  uint8_t day;     /* 1-31 */
  uint8_t hour;    /* 0-23 */
  uint8_t minute;
  uint8_t second;
} rtc_time_t;

int rtc_read(rtc_time_t *out); /* 1 ok, 0 = no clock */
void rtc_format_date(const rtc_time_t *t, char *buf, int bufsz);
void rtc_format_time(const rtc_time_t *t, char *buf, int bufsz);

#endif /* NAOS_RTC_H */
