#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


//  version information
#define RSC_VERSION_MAJOR     1
#define RSC_VERSION_MINOR     1
#define RSC_VERSION_RELEASE   0
#define RSC_VERSION           10100     // major * 10000 + minor * 100 + patch
#define RSC_VERSION_STRING    "1.1.0"


#define RSC_DEFAULT_TOUT      200
#define RSC_ONE_CHAR_TOUT     100
#define RSC_BUFFER_TOUT       500
#define RSC_BLOCKING          UINT32_MAX

#define RSC_MSG_Q_DEPTH       16

#define RSC_ENVELOPE_BYTE1    0xAA
#define RSC_ENVELOPE_BYTE2    0x55

// Sizes on the wire, in bytes
#define RSC_ENVELOPE_HEADER_LEN   6     // 0xAA, 0x55, payload length, inverted payload length
#define RSC_MESSAGE_HEADER_LEN    7     // type, CID, data length
#define RSC_TRAILER_LEN           2     // CRC-16
#define RSC_EMB_DATA_LEN          4     // data of 4 bytes or less is padded to 4 bytes
#define RSC_MAX_DATA_LEN          (0xFFFF - RSC_MESSAGE_HEADER_LEN)   // 65528: payload length is 16 bits

// Build options ===============================================================
// Number of ACK/NACK replies held for rscWaitForReply(). When full, the oldest is replaced.
#ifndef RSC_REPLY_SLOTS
#define RSC_REPLY_SLOTS       4
#endif
#if RSC_REPLY_SLOTS < 1
#error "RSC_REPLY_SLOTS must be 1 or more"
#endif

// Delay between two checks of rscWaitForReply(), in ms
#ifndef RSC_WAIT_POLL_MS
#define RSC_WAIT_POLL_MS      1
#endif


// Message types
#define RSC_MSG_TYPE_DATA     0xDA
#define RSC_MSG_TYPE_ACK      0xA0
#define RSC_MSG_TYPE_NACK     0xAE

#define RSC_MSG_NACK_RESEND   0x01
#define RSC_MSG_NACK_KEEP     0x00

typedef int16_t               rscError_t;
// General errors
#define RSC_OK                0
#define RSC_ERROR_GENERAL     (-1)
#define RSC_ERROR_TIMEOUT     (-2)
#define RSC_ERROR_OVERFLOW    (-3)
#define RSC_ERROR_NULL        (-4)
#define RSC_ERROR_MEMORY      (-5)

// Transmit errors
#define RSC_ERROR_TRX_Q_FULL  (-20)
#define RSC_ERROR_TRX_Q_EMPTY (-21)
#define RSC_ERROR_TRX_Q_ERROR (-22)
#define RSC_ERROR_TRX_ERROR   (-23)
#define RSC_ERROR_TRX_TIMEOUT (-24)

// Receive errors
#define RSC_ERROR_REC_BAD_LEN (-30)
#define RSC_ERROR_REC_OOM     (-31)
#define RSC_ERROR_REC_TIMEOUT (-32) //  data timeout
#define RSC_ERROR_REC_CRC     (-33)
#define RSC_ERROR_REC_Q_FULL  (-34)
#define RSC_ERROR_REC_Q_EMPTY (-35)
#define RSC_ERROR_REC_Q_ERROR (-36)
#define RSC_ERROR_REC_ERROR   (-37)

// Command errors (for use in NACK replies)
#define RSC_ERROR_CMD_BAD_ARG (-60)
#define RSC_ERROR_CMD_NOT_IMP (-61)   // command not [yet] implemented


#pragma pack (push, 1)

typedef struct {
  uint8_t     rscEnvID1;
  uint8_t     rscEnvID2;
  uint16_t    rscPayloadLen;
  uint16_t    rscInvPayloadLen;
} rscEnvelopeHeader_t;


typedef struct {
  uint8_t     rscMessageType;
  uint32_t    rscMessageCID;
  uint16_t    rscPayloadDataLen;
} rscMessageHeader_t;

typedef union {
  uint8_t*  ptr;
  uint8_t   data[RSC_EMB_DATA_LEN];
  uint32_t  num;
} rscFlexStorage_t;

typedef struct {
  rscMessageHeader_t  rscPayloadHeader;
  rscFlexStorage_t    rscPayloadData;
} rscEnvelopePayload_t;

typedef struct {
  uint16_t    rscPayloadCRC16;
} rscEnvelopeTrailer_t;

typedef struct {
  rscEnvelopeHeader_t   rscEnvelopeHeader;
  rscEnvelopePayload_t  rscEnvelopePayload;
  rscEnvelopeTrailer_t  rscEnvelopeTrailer;
} rscEnvelope_t;

#pragma pack (pop)

// Reply to a message, as returned by rscWaitForReply()
typedef struct {
  uint8_t     type;       // RSC_MSG_TYPE_ACK or RSC_MSG_TYPE_NACK
  int8_t      status;     // ACK: status byte (0 if none). NACK: error byte
  bool        resend;     // NACK: the peer asks for the message again
} rscReply_t;


//  Platform implementation ===================================================
void*       rscPlatformMalloc(size_t length);
void        rscPlatformFree(void* ptr);

void        rscPlatformEnterCritical(void);
void        rscPlatformExitCritical(void);
int         rscPlatformSendByte(uint8_t onebyte);
int         rscPlatformSendBytes(uint8_t* bytes, size_t length);

bool        rscPlatformReceiveReady(void);
int         rscPlatformReceiveByte(uint32_t timeout);
int         rscPlatformReceiveBytes(uint8_t* buffer, size_t expectedLength, uint32_t timeout);

void        rscPlatformDelayMs(uint32_t ms);
uint32_t    rscPlatformMillis(void);

rscError_t  rscPlatformEnqueueTransmitData( rscEnvelope_t* qData, uint32_t timeout);
rscError_t  rscPlatformDequeueTransmitData( rscEnvelope_t* qData, uint32_t timeout);

rscError_t  rscPlatformEnqueueReceiveData( rscEnvelope_t* qData, uint32_t timeout);
rscError_t  rscPlatformDequeueReceiveData( rscEnvelope_t* qData, uint32_t timeout);



// Message implementation =====================================================
uint32_t    rscGetNextCID(void);

rscError_t  rscSendStatusAck(uint32_t CID, int8_t status);
rscError_t  rscSendStatusNack(uint32_t CID, int8_t er, bool resend);

// Wait for the ACK or NACK of CID. RSC_OK and *reply, or RSC_ERROR_TIMEOUT.
// Timeout 0 checks once without a delay.
rscError_t  rscWaitForReply(uint32_t CID, rscReply_t* reply, uint32_t timeout);

// As rscWaitForReply(). *rc: ACK status (RSC_OK if none) or NACK error (RSC_ERROR_GENERAL if 0)
rscError_t  rscWaitForAckNack(uint32_t CID, rscError_t* rc, uint32_t timeout);


// Transmit implementation  ===================================================
bool        rscTransmitLoopReady(rscEnvelope_t* queueData, uint32_t timeout);
rscError_t  rscTransmitLoopSend(rscEnvelope_t* queueData);
void        rscTransmitLoop(void);

bool        rscReceiveLoopEnvelopeStart(uint32_t timeout);
rscError_t  rscReceiveLoopEnvelopeRead(void);
void        rscReceiveLoop(void);

rscError_t  rscSendMessage( const rscMessageHeader_t* msgHeader, const uint8_t* msgData );

// Send, wait up to timeout for the reply, and send again (same CID) on a timeout or a NACK
// that asks for a resend. RSC_OK when a reply arrived (reply->type tells ACK from NACK),
// RSC_ERROR_TIMEOUT after the last attempt, or the send error.
rscError_t  rscSendMessageReliable( const rscMessageHeader_t* msgHeader, const uint8_t* msgData,
                                    uint8_t attempts, uint32_t timeout, rscReply_t* reply );

// Non-blocking alternatives to rscReceiveLoop() and rscTransmitLoop(), for a superloop.
// Do not mix them with the loops.
rscError_t  rscReceivePoll(void);   // RSC_OK, or the error of the last frame rejected in this call
rscError_t  rscTransmitPoll(void);  // RSC_OK (one sent), RSC_ERROR_TRX_Q_EMPTY, or the send error


//  External references =======================================================
extern rscError_t rscReceiveError;

#ifdef __cplusplus
}
#endif

#ifndef RSC_NO_LEGACY
#include "rsc_legacy.h"
#endif
