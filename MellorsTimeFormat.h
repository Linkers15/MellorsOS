#pragma once
#include <stddef.h>
#include <time.h>

// Only the fixed formats exposed by MScript; no general strftime dependency.
void mellorsFormatDateTime(char* out, size_t capacity, const struct tm& value,
                          bool timeOnly, bool clock24h, bool showSeconds);
