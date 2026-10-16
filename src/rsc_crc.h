#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//Tested
#define RSC_CRC16_DNP	0x3D65		// DNP, IEC 870, M-BUS, wM-BUS, ...
#define RSC_CRC16_CCITT	0x1021		// X.25, V.41, HDLC FCS, Bluetooth, ...

//Other polynoms not tested
#define RSC_CRC16_IBM	0x8005		// ModBus, USB, Bisync, CRC-16, CRC-16-ANSI, ...
#define RSC_CRC16_T10_DIF	0x8BB7		// SCSI DIF
#define RSC_CRC16_DECT	0x0589		// Cordeless Telephones
#define RSC_CRC16_ARINC	0xA02B		// ACARS Aplications

#define RSC_CRC16_INIT  0xFFFF

// Polynomial used by rscCrc16value(). Both peers must use the same one.
// Override with -DRSC_CRC16_POLYNOM=RSC_CRC16_DNP (or any value above).
#ifndef RSC_CRC16_POLYNOM
#define RSC_CRC16_POLYNOM   RSC_CRC16_CCITT
#endif

uint16_t rscCrc16value( uint16_t crcValue, uint8_t newByte );
uint16_t rscCrc16( const uint8_t* bytes, size_t length, uint16_t crcInitialValue );


#ifdef __cplusplus
}
#endif
