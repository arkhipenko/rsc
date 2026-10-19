#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR.
// The check value of "123456789" is 0x29B1.
#define RSC_CRC16_INIT        0xFFFF

// Implementation, chosen at build time. All three give the same result.
//   0 - bitwise: no table, slowest
//   1 - nibble table: 16 entries (32 bytes), the AVR default
//   2 - byte table: 256 entries (512 bytes), the default elsewhere
#ifndef RSC_CRC_IMPL
#if defined(__AVR__)
#define RSC_CRC_IMPL          1
#else
#define RSC_CRC_IMPL          2
#endif
#endif

#if RSC_CRC_IMPL < 0 || RSC_CRC_IMPL > 2
#error "RSC_CRC_IMPL must be 0, 1 or 2"
#endif

// CRC of length bytes, continued from crc (RSC_CRC16_INIT for a new CRC)
uint16_t rscCrc16( const uint8_t* bytes, size_t length, uint16_t crc );

#ifdef __cplusplus
}
#endif
