#ifndef M68K_TYPES_H
#define M68K_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* Place the interpreter's hottest functions in IRAM on the ESP32-S3 so they
 * never pay a flash I-cache miss. No-op on the host build. */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define M68K_HOT IRAM_ATTR
#else
#define M68K_HOT
#endif

#endif
