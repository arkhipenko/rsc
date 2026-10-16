#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "rsc_crc.h"
#include "rsc_framework.h"

uint32_t rscMessageCIDCounter = 0;
rscError_t rscReceiveError;

uint32_t rscGetNextCID(void) {
  rscPlatformEnterCritical();
  uint32_t r = rscMessageCIDCounter++;
  rscPlatformExitCritical();
  return r;
}

// Wire encoding  =============================================================
// Every multi-byte field is little-endian on the wire, whatever the host byte order.
static void rscPut16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

static void rscPut32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}

static uint16_t rscGet16(const uint8_t* p) {
  return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rscGet32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void rscPackMessageHeader(uint8_t* p, const rscMessageHeader_t* h) {
  p[0] = h->rscMessageType;
  rscPut32(p + 1, h->rscMessageCID);
  rscPut16(p + 5, h->rscPayloadDataLen);
}

static void rscUnpackMessageHeader(rscMessageHeader_t* h, const uint8_t* p) {
  h->rscMessageType = p[0];
  h->rscMessageCID = rscGet32(p + 1);
  h->rscPayloadDataLen = rscGet16(p + 5);
}

// Number of data bytes on the wire: data of 4 bytes or less is padded to 4
static size_t rscWireDataLen(uint16_t dataLen) {
  return (dataLen < RSC_EMB_DATA_LEN) ? RSC_EMB_DATA_LEN : dataLen;
}

// Data longer than 4 bytes is held in a heap buffer, shorter data inside the envelope
static bool rscHeapPayload(const rscEnvelope_t* e) {
  return e->rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen > RSC_EMB_DATA_LEN;
}

static uint8_t* rscPayloadBytes(rscEnvelope_t* e) {
  if ( rscHeapPayload(e) ) return e->rscEnvelopePayload.rscPayloadData.ptr;
  return e->rscEnvelopePayload.rscPayloadData.data;
}

static void rscFreePayload(rscEnvelope_t* e) {
  if ( rscHeapPayload(e) ) {
    rscPlatformFree(e->rscEnvelopePayload.rscPayloadData.ptr);
    e->rscEnvelopePayload.rscPayloadData.ptr = NULL;
  }
}

// CRC-16 over the message header (as sent) and the data bytes (as sent)
static uint16_t rscEnvelopeCrc(rscEnvelope_t* e) {
  uint8_t h[RSC_MESSAGE_HEADER_LEN];
  rscPackMessageHeader(h, &e->rscEnvelopePayload.rscPayloadHeader);
  uint16_t c = rscCrc16( h, RSC_MESSAGE_HEADER_LEN, RSC_CRC16_INIT );
  return rscCrc16( rscPayloadBytes(e), rscWireDataLen(e->rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen), c );
}

// The inverted length must be the bitwise NOT of the length, and the length must hold
// at least a message header and the 4 padded data bytes
static bool rscEnvelopeLengthValid(uint16_t len, uint16_t inv) {
  return (uint16_t) (len ^ inv) == 0xFFFF && len >= RSC_MESSAGE_HEADER_LEN + RSC_EMB_DATA_LEN;
}

// The length must match the data length announced by the message header
static bool rscPayloadLengthValid(uint16_t len, uint16_t dataLen) {
  return (uint32_t) len == (uint32_t) (RSC_MESSAGE_HEADER_LEN + rscWireDataLen(dataLen));
}


// Replies  ===================================================================
// ACK and NACK frames are kept here for rscWaitForReply(), not in the receive queue.
typedef struct {
  uint32_t    cid;
  uint32_t    seq;
  rscReply_t  reply;
  bool        used;
} rscReplySlot_t;

static rscReplySlot_t rscReplies[RSC_REPLY_SLOTS];
static uint32_t rscReplySeq = 0;

static void rscStoreReply(uint32_t cid, const rscReply_t* r) {
  int slot = -1;

  rscPlatformEnterCritical();
  // a reply for the same CID replaces the earlier one, then a free slot, then the oldest
  for (int i = 0; i < RSC_REPLY_SLOTS && slot < 0; i++) {
    if ( rscReplies[i].used && rscReplies[i].cid == cid ) slot = i;
  }
  for (int i = 0; i < RSC_REPLY_SLOTS && slot < 0; i++) {
    if ( !rscReplies[i].used ) slot = i;
  }
  if ( slot < 0 ) {
    slot = 0;
    for (int i = 1; i < RSC_REPLY_SLOTS; i++) {
      if ( (int32_t) (rscReplies[i].seq - rscReplies[slot].seq) < 0 ) slot = i;
    }
  }
  rscReplies[slot].cid = cid;
  rscReplies[slot].seq = ++rscReplySeq;
  rscReplies[slot].reply = *r;
  rscReplies[slot].used = true;
  rscPlatformExitCritical();
}

static bool rscTakeReply(uint32_t cid, rscReply_t* r) {
  bool found = false;

  rscPlatformEnterCritical();
  for (int i = 0; i < RSC_REPLY_SLOTS; i++) {
    if ( rscReplies[i].used && rscReplies[i].cid == cid ) {
      *r = rscReplies[i].reply;
      rscReplies[i].used = false;
      found = true;
      break;
    }
  }
  rscPlatformExitCritical();
  return found;
}

// Route a valid frame: ACK and NACK to the reply table, everything else to the receive queue.
// Takes ownership of a heap payload.
static rscError_t rscDeliver(rscEnvelope_t* e) {
  rscMessageHeader_t* h = &e->rscEnvelopePayload.rscPayloadHeader;

  if ( h->rscMessageType == RSC_MSG_TYPE_ACK || h->rscMessageType == RSC_MSG_TYPE_NACK ) {
    const uint8_t* d = rscPayloadBytes(e);
    rscReply_t r;
    r.type = h->rscMessageType;
    r.status = ( h->rscPayloadDataLen >= 1 ) ? (int8_t) d[0] : 0;
    r.resend = ( h->rscMessageType == RSC_MSG_TYPE_NACK && h->rscPayloadDataLen >= 2 && d[1] == RSC_MSG_NACK_RESEND );
    rscStoreReply(h->rscMessageCID, &r);
    rscFreePayload(e);
    return RSC_OK;
  }

  if ( rscPlatformEnqueueReceiveData(e, RSC_DEFAULT_TOUT) != RSC_OK ) {
    rscFreePayload(e);
    return RSC_ERROR_REC_Q_FULL;
  }
  return RSC_OK;
}

// Check the CRC of a complete frame and deliver it. Takes ownership of a heap payload.
static rscError_t rscFinishFrame(rscEnvelope_t* e) {
  if ( e->rscEnvelopeTrailer.rscPayloadCRC16 != rscEnvelopeCrc(e) ) {
    rscFreePayload(e);
    return RSC_ERROR_REC_CRC;
  }
  return rscDeliver(e);
}


// Message implementation =====================================================
rscError_t rscSendStatusAck(uint32_t CID, int8_t status) {
  rscMessageHeader_t m;
  uint8_t s = (uint8_t) status;
  m.rscMessageType = RSC_MSG_TYPE_ACK;
  m.rscMessageCID = CID;
  if ( status == 0 ) {
    m.rscPayloadDataLen = 0;
  }
  else {
    m.rscPayloadDataLen = 1;
  }
  return rscSendMessage(&m, &s);
}

rscError_t rscSendStatusNack(uint32_t CID, int8_t er, bool resend) {
  rscMessageHeader_t m;
  m.rscMessageType = RSC_MSG_TYPE_NACK;
  m.rscMessageCID = CID;
  m.rscPayloadDataLen = 2;

  uint8_t d[2];
  d[0] = (uint8_t) er;
  d[1] = resend ? RSC_MSG_NACK_RESEND : RSC_MSG_NACK_KEEP;
  return rscSendMessage(&m, d);
}

rscError_t rscWaitForReply(uint32_t CID, rscReply_t* reply, uint32_t timeout) {
  if ( reply == NULL ) return RSC_ERROR_NULL;

  uint32_t st = rscPlatformMillis();
  while (true) {
    if ( rscTakeReply(CID, reply) ) return RSC_OK;
    if ( timeout == 0 ) return RSC_ERROR_TIMEOUT;
    if ( timeout != RSC_BLOCKING && rscPlatformMillis() - st > timeout ) return RSC_ERROR_TIMEOUT;
    rscPlatformDelayMs(RSC_WAIT_POLL_MS);
  }
}

rscError_t rscWaitForAckNack(uint32_t CID, rscError_t* rc, uint32_t timeout) {
  rscReply_t r;

  if ( rc == NULL ) return RSC_ERROR_NULL;
  rscError_t res = rscWaitForReply(CID, &r, timeout);
  if ( res != RSC_OK ) return res;

  if ( r.type == RSC_MSG_TYPE_ACK ) *rc = r.status;
  else *rc = ( r.status != 0 ) ? r.status : RSC_ERROR_GENERAL;  // a NACK never reads as success
  return RSC_OK;
}


// Transmit implementation  ===================================================
rscError_t  rscSendMessage( const rscMessageHeader_t* msgHeader, const uint8_t* msgData ) {
  rscEnvelope_t e;

  if ( msgHeader == NULL ) return RSC_ERROR_NULL;
  uint16_t dataLen = msgHeader->rscPayloadDataLen;
  if ( dataLen > 0 &&  msgData == NULL ) return RSC_ERROR_NULL;
  if ( dataLen > RSC_MAX_DATA_LEN ) return RSC_ERROR_OVERFLOW;

  memset(&e, 0, sizeof(rscEnvelope_t));

  // populate header
  e.rscEnvelopeHeader.rscEnvID1 = RSC_ENVELOPE_BYTE1;
  e.rscEnvelopeHeader.rscEnvID2 = RSC_ENVELOPE_BYTE2;
  e.rscEnvelopeHeader.rscPayloadLen = (uint16_t) (RSC_MESSAGE_HEADER_LEN + rscWireDataLen(dataLen));
  e.rscEnvelopeHeader.rscInvPayloadLen = (uint16_t) ~e.rscEnvelopeHeader.rscPayloadLen;

  memcpy( (void*) &e.rscEnvelopePayload.rscPayloadHeader, msgHeader, sizeof(rscMessageHeader_t) );

  // if data payload is small (4 bytes or fewer) - use the pointer space - avoid allocating 1 byte long areas
  // for every ACK/NACK message
  if ( rscHeapPayload(&e) ) {
    e.rscEnvelopePayload.rscPayloadData.ptr = (uint8_t*) rscPlatformMalloc(dataLen);
    if ( e.rscEnvelopePayload.rscPayloadData.ptr == NULL ) return RSC_ERROR_MEMORY;
    memcpy( (void*) e.rscEnvelopePayload.rscPayloadData.ptr, msgData, dataLen );
  }
  else {
    for (uint16_t i = 0; i < dataLen; i++) e.rscEnvelopePayload.rscPayloadData.data[i] = msgData[i];
  }

  e.rscEnvelopeTrailer.rscPayloadCRC16 = rscEnvelopeCrc(&e);

  rscError_t rc = rscPlatformEnqueueTransmitData(&e, RSC_DEFAULT_TOUT);
  if ( rc != RSC_OK ) rscFreePayload(&e);
  return rc;
}

rscError_t rscSendMessageReliable( const rscMessageHeader_t* msgHeader, const uint8_t* msgData,
                                   uint8_t attempts, uint32_t timeout, rscReply_t* reply ) {
  rscReply_t r;

  if ( msgHeader == NULL ) return RSC_ERROR_NULL;
  if ( attempts == 0 ) attempts = 1;

  for (uint8_t i = 0; i < attempts; i++) {
    rscError_t rc = rscSendMessage(msgHeader, msgData);
    if ( rc != RSC_OK ) return rc;

    if ( rscWaitForReply(msgHeader->rscMessageCID, &r, timeout) == RSC_OK ) {
      if ( reply ) *reply = r;
      if ( r.type == RSC_MSG_TYPE_NACK && r.resend && i + 1 < attempts ) continue;
      return RSC_OK;
    }
  }
  return RSC_ERROR_TIMEOUT;
}

bool rscTransmitLoopReady(rscEnvelope_t* qD, uint32_t timeout) {
  return rscPlatformDequeueTransmitData( qD, timeout) == RSC_OK;
}

rscError_t rscTransmitLoopSend(rscEnvelope_t* qD) {
  uint8_t h[RSC_ENVELOPE_HEADER_LEN + RSC_MESSAGE_HEADER_LEN];
  uint8_t c[RSC_TRAILER_LEN];
  int n;

  // send envelope header and message header
  h[0] = qD->rscEnvelopeHeader.rscEnvID1;
  h[1] = qD->rscEnvelopeHeader.rscEnvID2;
  rscPut16(h + 2, qD->rscEnvelopeHeader.rscPayloadLen);
  rscPut16(h + 4, qD->rscEnvelopeHeader.rscInvPayloadLen);
  rscPackMessageHeader(h + RSC_ENVELOPE_HEADER_LEN, &qD->rscEnvelopePayload.rscPayloadHeader);
  n = rscPlatformSendBytes( h, sizeof(h) );
  if ( n != (int) sizeof(h) ) return RSC_ERROR_GENERAL;

  // send data
  size_t dLen = rscWireDataLen(qD->rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen);
  n = rscPlatformSendBytes( rscPayloadBytes(qD), dLen );
  if ( n < 0 || (size_t) n != dLen ) return RSC_ERROR_GENERAL;

  // send CRC
  rscPut16(c, qD->rscEnvelopeTrailer.rscPayloadCRC16);
  n = rscPlatformSendBytes( c, sizeof(c) );
  if ( n != (int) sizeof(c) ) return RSC_ERROR_GENERAL;

  return RSC_OK;
}

// Send one queued envelope and free its payload memory after it is sent
static rscError_t rscTransmitOne(uint32_t timeout) {
  rscEnvelope_t qd;

  memset(&qd, 0, sizeof(rscEnvelope_t));
  rscError_t rc = rscPlatformDequeueTransmitData( &qd, timeout );
  if ( rc != RSC_OK ) return rc;

  rc = rscTransmitLoopSend(&qd);
  rscFreePayload(&qd);
  return rc;
}

void rscTransmitLoop(void) {
  while (true) {
    rscTransmitOne(RSC_BLOCKING);
  }
}

rscError_t rscTransmitPoll(void) {
  return rscTransmitOne(0);
}



// Data receipt implementation  ===============================================
bool rscReceiveLoopEnvelopeStart(uint32_t timeout) {

  int c = rscPlatformReceiveByte( timeout ); // wait for a byte
  if ( c < 0 ) return false;
  if ( (uint8_t) (c & 0xFF) != RSC_ENVELOPE_BYTE1 ) return false;

  while (true) {
    c = rscPlatformReceiveByte( RSC_ONE_CHAR_TOUT ); // wait for a byte
    if ( c < 0 ) return false;
    if ( (uint8_t) (c & 0xFF) == RSC_ENVELOPE_BYTE2 ) return true;
    if ( (uint8_t) (c & 0xFF) == RSC_ENVELOPE_BYTE1 ) continue;
    return false;
  }
}

rscError_t rscReceiveLoopEnvelopeRead(void) {
  rscEnvelope_t e;
  uint8_t b[RSC_MESSAGE_HEADER_LEN];

  memset(&e, 0, sizeof(rscEnvelope_t));

  e.rscEnvelopeHeader.rscEnvID1 = RSC_ENVELOPE_BYTE1;
  e.rscEnvelopeHeader.rscEnvID2 = RSC_ENVELOPE_BYTE2;

  if ( rscPlatformReceiveBytes( b, sizeof(uint16_t), RSC_DEFAULT_TOUT ) != (int) sizeof(uint16_t) ) return RSC_ERROR_REC_ERROR;
  e.rscEnvelopeHeader.rscPayloadLen = rscGet16(b);
  if ( rscPlatformReceiveBytes( b, sizeof(uint16_t), RSC_DEFAULT_TOUT ) != (int) sizeof(uint16_t) ) return RSC_ERROR_REC_ERROR;
  e.rscEnvelopeHeader.rscInvPayloadLen = rscGet16(b);

  // check the lengths
  if ( !rscEnvelopeLengthValid(e.rscEnvelopeHeader.rscPayloadLen, e.rscEnvelopeHeader.rscInvPayloadLen) ) return RSC_ERROR_REC_BAD_LEN;

  if ( rscPlatformReceiveBytes( b, RSC_MESSAGE_HEADER_LEN, RSC_DEFAULT_TOUT ) != RSC_MESSAGE_HEADER_LEN ) return RSC_ERROR_REC_ERROR;
  rscUnpackMessageHeader(&e.rscEnvelopePayload.rscPayloadHeader, b);

  // check lengths again: total payload length should be equal to payload header length + (padded) payload data length
  uint16_t dataLen = e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen;
  if ( !rscPayloadLengthValid(e.rscEnvelopeHeader.rscPayloadLen, dataLen) ) return RSC_ERROR_REC_BAD_LEN;

  size_t pLen = rscWireDataLen(dataLen);
  if ( rscHeapPayload(&e) ) {
    e.rscEnvelopePayload.rscPayloadData.ptr = (uint8_t*) rscPlatformMalloc(dataLen);
    if ( e.rscEnvelopePayload.rscPayloadData.ptr == NULL ) return RSC_ERROR_REC_OOM;
  }
  int n = rscPlatformReceiveBytes( rscPayloadBytes(&e), pLen, RSC_BUFFER_TOUT );
  if ( n < 0 || (size_t) n != pLen ) {
    rscFreePayload(&e);
    return RSC_ERROR_REC_TIMEOUT;
  }

  if ( rscPlatformReceiveBytes( b, RSC_TRAILER_LEN, RSC_DEFAULT_TOUT ) != RSC_TRAILER_LEN ) {
    rscFreePayload(&e);
    return RSC_ERROR_REC_TIMEOUT;
  }
  e.rscEnvelopeTrailer.rscPayloadCRC16 = rscGet16(b);

  return rscFinishFrame(&e);
}

void rscReceiveLoop(void) {
  rscReceiveError = RSC_OK;

  while (true) {
    while( !rscPlatformReceiveReady() );
    if ( rscReceiveLoopEnvelopeStart(RSC_BLOCKING) ) {
      rscReceiveError = rscReceiveLoopEnvelopeRead();
    }
  }
}


// Non-blocking receive  ======================================================
// A byte-driven parser for rscReceivePoll(). It validates frames the same way as
// rscReceiveLoopEnvelopeRead().
enum {
  RSC_RX_START1 = 0,
  RSC_RX_START2,
  RSC_RX_LENGTH,
  RSC_RX_HEADER,
  RSC_RX_DATA,
  RSC_RX_CRC
};

static struct {
  uint8_t       state;
  uint8_t       raw[RSC_MESSAGE_HEADER_LEN];  // length fields, message header or CRC
  size_t        got;
  size_t        need;
  bool          heap;                         // e holds a heap payload
  uint32_t      last;                         // time of the last byte
  rscEnvelope_t e;
} rscRx;

static void rscRxReset(void) {
  if ( rscRx.heap ) rscFreePayload(&rscRx.e);
  rscRx.heap = false;
  rscRx.state = RSC_RX_START1;
  rscRx.got = 0;
}

static rscError_t rscRxByte(uint8_t c) {
  switch (rscRx.state) {
    case RSC_RX_START1:
      if ( c == RSC_ENVELOPE_BYTE1 ) rscRx.state = RSC_RX_START2;
      return RSC_OK;

    case RSC_RX_START2:
      if ( c == RSC_ENVELOPE_BYTE2 ) {
        memset(&rscRx.e, 0, sizeof(rscEnvelope_t));
        rscRx.e.rscEnvelopeHeader.rscEnvID1 = RSC_ENVELOPE_BYTE1;
        rscRx.e.rscEnvelopeHeader.rscEnvID2 = RSC_ENVELOPE_BYTE2;
        rscRx.got = 0;
        rscRx.state = RSC_RX_LENGTH;
      }
      else if ( c != RSC_ENVELOPE_BYTE1 ) rscRx.state = RSC_RX_START1;
      return RSC_OK;

    case RSC_RX_LENGTH:
      rscRx.raw[rscRx.got++] = c;
      if ( rscRx.got < 2 * sizeof(uint16_t) ) return RSC_OK;
      rscRx.e.rscEnvelopeHeader.rscPayloadLen = rscGet16(rscRx.raw);
      rscRx.e.rscEnvelopeHeader.rscInvPayloadLen = rscGet16(rscRx.raw + 2);
      if ( !rscEnvelopeLengthValid(rscRx.e.rscEnvelopeHeader.rscPayloadLen, rscRx.e.rscEnvelopeHeader.rscInvPayloadLen) ) {
        rscRxReset();
        return RSC_ERROR_REC_BAD_LEN;
      }
      rscRx.got = 0;
      rscRx.state = RSC_RX_HEADER;
      return RSC_OK;

    case RSC_RX_HEADER:
      rscRx.raw[rscRx.got++] = c;
      if ( rscRx.got < RSC_MESSAGE_HEADER_LEN ) return RSC_OK;
      rscUnpackMessageHeader(&rscRx.e.rscEnvelopePayload.rscPayloadHeader, rscRx.raw);
      if ( !rscPayloadLengthValid(rscRx.e.rscEnvelopeHeader.rscPayloadLen, rscRx.e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen) ) {
        rscRxReset();
        return RSC_ERROR_REC_BAD_LEN;
      }
      if ( rscHeapPayload(&rscRx.e) ) {
        rscRx.e.rscEnvelopePayload.rscPayloadData.ptr = (uint8_t*) rscPlatformMalloc(rscRx.e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen);
        if ( rscRx.e.rscEnvelopePayload.rscPayloadData.ptr == NULL ) {
          rscRxReset();
          return RSC_ERROR_REC_OOM;
        }
        rscRx.heap = true;
      }
      rscRx.need = rscWireDataLen(rscRx.e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen);
      rscRx.got = 0;
      rscRx.state = RSC_RX_DATA;
      return RSC_OK;

    case RSC_RX_DATA:
      rscPayloadBytes(&rscRx.e)[rscRx.got++] = c;
      if ( rscRx.got == rscRx.need ) {
        rscRx.got = 0;
        rscRx.state = RSC_RX_CRC;
      }
      return RSC_OK;

    case RSC_RX_CRC:
    default:
      rscRx.raw[rscRx.got++] = c;
      if ( rscRx.got < RSC_TRAILER_LEN ) return RSC_OK;
      rscRx.e.rscEnvelopeTrailer.rscPayloadCRC16 = rscGet16(rscRx.raw);
      rscRx.heap = false;                 // rscFinishFrame() takes the payload
      rscRx.state = RSC_RX_START1;
      rscRx.got = 0;
      return rscFinishFrame(&rscRx.e);
  }
}

rscError_t rscReceivePoll(void) {
  rscError_t result = RSC_OK;

  // a frame that stalls for longer than RSC_DEFAULT_TOUT is dropped
  if ( rscRx.state != RSC_RX_START1 && rscPlatformMillis() - rscRx.last > RSC_DEFAULT_TOUT ) {
    rscRxReset();
    result = RSC_ERROR_REC_TIMEOUT;
  }

  while ( rscPlatformReceiveReady() ) {
    int c = rscPlatformReceiveByte(0);
    if ( c < 0 ) break;
    rscRx.last = rscPlatformMillis();
    rscError_t rc = rscRxByte( (uint8_t) (c & 0xFF) );
    if ( rc != RSC_OK ) result = rc;
  }
  return result;
}
