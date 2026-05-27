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

/* Machine model the emulator is configured for. The Plus path is the
 * historical default (68000, 24-bit bus, Plus MMIO map); SE/30 turns on
 * 68020/030 ISA, 32-bit bus, and the SE/30 MMIO map. */
typedef enum {
    MAC_MODEL_PLUS = 0,
    MAC_MODEL_SE30 = 1,
} mac_machine_t;

#endif
