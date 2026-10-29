#include "MellorsTimeFormat.h"
#include <stdio.h>

// Ordinary .text goes to flash on ESP32 2.0.17. Keep this out of ISR paths.
// The SDK places newlib strftime in IRAM; fixed numeric formatting avoids it.
void mellorsFormatDateTime(char* out, size_t capacity, const struct tm& value,
                          bool timeOnly, bool clock24h, bool showSeconds) {
  if (!timeOnly) {
    snprintf(out, capacity, "%04d-%02d-%02d", value.tm_year + 1900,
             value.tm_mon + 1, value.tm_mday);
    return;
  }
  const int hour = clock24h ? value.tm_hour : (value.tm_hour % 12 ? value.tm_hour % 12 : 12);
  const char* suffix = clock24h ? "" : (value.tm_hour < 12 ? " AM" : " PM");
  if (showSeconds) {
    snprintf(out, capacity, "%02d:%02d:%02d%s", hour, value.tm_min, value.tm_sec, suffix);
  } else {
    snprintf(out, capacity, "%02d:%02d%s", hour, value.tm_min, suffix);
  }
}
