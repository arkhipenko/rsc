#pragma once

// Names from ReliableSerialComms 1.0.0, kept for existing code.
// rsc_framework.h includes this file unless RSC_NO_LEGACY is defined.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Misspelled names. An application that defines rscPlatfromSendBytes() keeps working
// when that source file includes rsc_framework.h first.
#define rscPlatfromSendBytes  rscPlatformSendBytes
#define rscTrasmitLoopSend    rscTransmitLoopSend

#define RSC_MSG_TYPE_RES      RSC_MSG_TYPE_ACK    // same value as ACK in 1.0.0 (0xA0)
#define RSC_ERROR_REC_BADLEN  (-38)               // never returned; use RSC_ERROR_REC_BAD_LEN
#define RSC_ERROR_CMD_FLASH   (-62)

// Application items (relay status), not used by the library
#define RSC_MSG_RESET_SOFT    0 //  just a software reset
#define RSC_MSG_RESET_FACT    1 //  erase mfg data and reset

#pragma pack (push, 1)

typedef struct {
	uint8_t    major;
	uint8_t    minor;
	uint16_t  release;
} rscVersionComponents_t;

typedef union {
  rscVersionComponents_t  cmp;
  uint32_t                num;
} rscVersion_t;

typedef struct {
  rscVersion_t  rscVersion;
  uint8_t       rscStatusMfg;
  uint8_t       rscStatusBLEConnected;
  uint8_t       rscStatusLoRaConnected;
  int16_t       rscLastError;
} rscRelayStatus_t;

#pragma pack (pop)

#ifdef __cplusplus
}
#endif
