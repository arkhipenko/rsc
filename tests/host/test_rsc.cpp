// Host tests for ReliableSerialComms, against the loopback platform model in stubs/.
// Run all tests, or one: ./test_rsc <test name>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "rsc_framework.h"
#include "rsc_crc.h"
#include "platform_model.h"

#ifndef RSC_REPO_DIR
#define RSC_REPO_DIR "../.."
#endif

static int failures = 0;

#define CHECK(cond) do { \
    if ( !(cond) ) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
  } while (0)

#define CHECK_EQ(a, b) do { \
    long long va_ = (long long) (a), vb_ = (long long) (b); \
    if ( va_ != vb_ ) { printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); failures++; } \
  } while (0)


// Helpers ====================================================================

// Move every queued envelope onto the wire (the transmit task)
static void pumpTransmit() {
  rscEnvelope_t qd;
  while ( rscTransmitLoopReady(&qd, 0) ) {
    rscTransmitLoopSend(&qd);
    if ( qd.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen > RSC_EMB_DATA_LEN ) rscPlatformFree(qd.rscEnvelopePayload.rscPayloadData.ptr);
  }
}

// Read one frame with the blocking reader (the receive task)
static rscError_t receiveFrame() {
  if ( !rscReceiveLoopEnvelopeStart(0) ) return RSC_ERROR_GENERAL;
  return rscReceiveLoopEnvelopeRead();
}

// Read every frame on the wire
static void receiveAll() {
  while ( modelWirePending() > 0 ) {
    if ( rscReceiveLoopEnvelopeStart(0) ) rscReceiveLoopEnvelopeRead();
  }
}

static rscError_t sendData(uint32_t cid, const uint8_t* data, uint16_t len) {
  rscMessageHeader_t h;
  h.rscMessageType = RSC_MSG_TYPE_DATA;
  h.rscMessageCID = cid;
  h.rscPayloadDataLen = len;
  return rscSendMessage(&h, data);
}

static const uint8_t* payloadOf(rscEnvelope_t& e) {
  if ( e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen > RSC_EMB_DATA_LEN ) return e.rscEnvelopePayload.rscPayloadData.ptr;
  return e.rscEnvelopePayload.rscPayloadData.data;
}

static void freePayload(rscEnvelope_t& e) {
  if ( e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen > RSC_EMB_DATA_LEN ) rscPlatformFree(e.rscEnvelopePayload.rscPayloadData.ptr);
}

static void put16(std::vector<uint8_t>& f, uint16_t v) {
  f.push_back((uint8_t) v);
  f.push_back((uint8_t) (v >> 8));
}

// Build a frame by hand. dataBytes are the bytes sent after the message header.
static std::vector<uint8_t> buildFrame(uint16_t len, uint16_t inv, uint8_t type, uint32_t cid, uint16_t dataLen,
                                       const std::vector<uint8_t>& dataBytes, bool goodCrc = true) {
  std::vector<uint8_t> f;
  f.push_back(RSC_ENVELOPE_BYTE1);
  f.push_back(RSC_ENVELOPE_BYTE2);
  put16(f, len);
  put16(f, inv);
  size_t h = f.size();
  f.push_back(type);
  for (int i = 0; i < 4; i++) f.push_back((uint8_t) (cid >> (8 * i)));
  put16(f, dataLen);
  f.insert(f.end(), dataBytes.begin(), dataBytes.end());
  uint16_t crc = rscCrc16(&f[h], f.size() - h, RSC_CRC16_INIT);
  if ( !goodCrc ) crc ^= 0x0101;
  put16(f, crc);
  return f;
}

// A valid DATA frame with n data bytes 0, 1, 2, ...
static std::vector<uint8_t> validFrame(uint32_t cid, uint16_t n, bool goodCrc = true) {
  std::vector<uint8_t> d(n < RSC_EMB_DATA_LEN ? RSC_EMB_DATA_LEN : n, 0);
  for (uint16_t i = 0; i < n; i++) d[i] = (uint8_t) i;
  uint16_t len = (uint16_t) (RSC_MESSAGE_HEADER_LEN + d.size());
  return buildFrame(len, (uint16_t) ~len, RSC_MSG_TYPE_DATA, cid, n, d, goodCrc);
}

static void push(const std::vector<uint8_t>& f) {
  modelWirePush(f.data(), f.size());
}

static bool wireEquals(const char* hex) {
  std::vector<uint8_t> expect;
  unsigned int b;
  int used;
  while ( sscanf(hex, "%2x%n", &b, &used) == 1 ) {
    expect.push_back((uint8_t) b);
    hex += used;
    while ( *hex == ' ' ) hex++;
  }
  return expect.size() == model.wHead && memcmp(expect.data(), model.wire, model.wHead) == 0;
}

static void roundtrip(uint16_t n) {
  std::vector<uint8_t> d(n);
  for (uint16_t i = 0; i < n; i++) d[i] = (uint8_t) (i * 7 + 1);
  CHECK_EQ(sendData(1000 + n, n ? d.data() : NULL, n), RSC_OK);
  pumpTransmit();
  CHECK_EQ(model.wHead, RSC_ENVELOPE_HEADER_LEN + RSC_MESSAGE_HEADER_LEN + (n < 4 ? 4 : n) + RSC_TRAILER_LEN);
  CHECK_EQ(receiveFrame(), RSC_OK);
  rscEnvelope_t e;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e, 0), RSC_OK);
  CHECK_EQ(e.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 1000 + n);
  CHECK_EQ(e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen, n);
  CHECK(n == 0 || memcmp(payloadOf(e), d.data(), n) == 0);
  freePayload(e);
}

static std::string readFile(const char* rel) {
  std::ifstream f(std::string(RSC_REPO_DIR) + "/" + rel);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}


// CRC and wire format ========================================================

static void test_crc_check_value() {
  const uint8_t s[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
  CHECK_EQ(rscCrc16(s, sizeof(s), RSC_CRC16_INIT), 0x29B1);   // CRC-16/CCITT-FALSE
}

// The worked examples of the spec: little-endian on every host
static void test_wire_golden_nack() {
  CHECK_EQ(rscSendStatusNack(42, (int8_t) RSC_ERROR_REC_CRC, true), RSC_OK);
  pumpTransmit();
  CHECK(wireEquals("AA 55 0B 00 F4 FF AE 2A 00 00 00 02 00 DF 01 00 00 F4 C9"));
}

static void test_wire_golden_data() {
  const uint8_t d[] = { 1, 2, 3, 4, 5, 6 };
  CHECK_EQ(sendData(16, d, 6), RSC_OK);
  pumpTransmit();
  CHECK(wireEquals("AA 55 0D 00 F2 FF DA 10 00 00 00 06 00 01 02 03 04 05 06 C3 3E"));
}

static void test_wire_golden_ack() {
  CHECK_EQ(rscSendStatusAck(42, 0), RSC_OK);
  pumpTransmit();
  CHECK(wireEquals("AA 55 0B 00 F4 FF A0 2A 00 00 00 00 00 00 00 00 00 13 5A"));
}

static void test_roundtrip_sizes() {
  const uint16_t sizes[] = { 0, 1, 3, 4, 5, 6, 300 };
  for (uint16_t n : sizes) {
    modelReset();
    roundtrip(n);
  }
  CHECK_EQ(model.allocs, model.frees);
}

static void test_resync_on_repeated_start_byte() {
  const uint8_t junk[] = { 0x00, 0x13, RSC_ENVELOPE_BYTE1 };
  modelWirePush(junk, sizeof(junk));
  push(validFrame(5, 2));
  receiveAll();
  CHECK_EQ(modelQueueCount(model.rx), 1);
}


// D3, D4, D5: length checks ==================================================

static void test_d3_inverse_length_checked() {
  std::vector<uint8_t> f = validFrame(9, 2);
  f[4] = 0x00;
  f[5] = 0x00;                       // inverse 0x0000 shares no bit with 0x000B
  push(f);
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_BAD_LEN);
  CHECK_EQ(modelQueueCount(model.rx), 0);
}

static void test_d4_length_above_embedded_data() {
  // length 71 with data length 2: 1.0.0 wrote 64 bytes into the 4-byte field on the stack
  std::vector<uint8_t> d(64, 0x5A);
  push(buildFrame(7 + 64, (uint16_t) ~(7 + 64), RSC_MSG_TYPE_DATA, 1, 2, d));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_BAD_LEN);
  CHECK_EQ(modelQueueCount(model.rx), 0);
}

static void test_d4_length_above_heap_data() {
  // length 47 with data length 10: 1.0.0 wrote 40 bytes into a 10-byte heap buffer
  std::vector<uint8_t> d(40, 0x5A);
  push(buildFrame(7 + 40, (uint16_t) ~(7 + 40), RSC_MSG_TYPE_DATA, 1, 10, d));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_BAD_LEN);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_d4_length_below_data() {
  std::vector<uint8_t> d(8, 0x5A);
  push(buildFrame(7 + 8, (uint16_t) ~(7 + 8), RSC_MSG_TYPE_DATA, 1, 20, d));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_BAD_LEN);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_d5_length_below_header() {
  // length 3 with a correct inverse: 1.0.0 computed 3 - 7 as a size_t
  std::vector<uint8_t> d(40, 0x77);
  push(buildFrame(3, (uint16_t) ~3, RSC_MSG_TYPE_DATA, 1, 0, d));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_BAD_LEN);
}

static void test_padded_length_accepted() {
  // data length 0..4 always travels as 4 bytes: length 11
  for (uint16_t n = 0; n <= 4; n++) {
    modelReset();
    push(validFrame(n, n));
    CHECK_EQ(receiveFrame(), RSC_OK);
    CHECK_EQ(modelQueueCount(model.rx), 1);
  }
}


// D6, D10, D11: sending ======================================================

static void test_d6_no_leak_when_transmit_queue_full() {
  uint8_t d[20] = { 0 };
  model.tx.capacity = 0;
  CHECK_EQ(sendData(1, d, sizeof(d)), RSC_ERROR_TRX_Q_FULL);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_d10_payload_above_32767() {
  static uint8_t d[40000];
  for (size_t i = 0; i < sizeof(d); i++) d[i] = (uint8_t) i;
  CHECK_EQ(sendData(1, d, sizeof(d)), RSC_OK);
  rscEnvelope_t qd;
  CHECK(rscTransmitLoopReady(&qd, 0));
  CHECK_EQ(rscTransmitLoopSend(&qd), RSC_OK);
  rscPlatformFree(qd.rscEnvelopePayload.rscPayloadData.ptr);
  CHECK_EQ(receiveFrame(), RSC_OK);
  rscEnvelope_t e;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e, 0), RSC_OK);
  CHECK(memcmp(payloadOf(e), d, sizeof(d)) == 0);
  freePayload(e);
}

static void test_d11_data_length_limit() {
  static uint8_t d[65535];
  CHECK_EQ(sendData(1, d, 65529), RSC_ERROR_OVERFLOW);
  CHECK_EQ(modelQueueCount(model.tx), 0);
  CHECK_EQ(sendData(1, d, 65535), RSC_ERROR_OVERFLOW);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_max_data_length_roundtrip() {
  roundtrip(65528);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_send_null_arguments() {
  rscMessageHeader_t h;
  h.rscMessageType = RSC_MSG_TYPE_DATA;
  h.rscMessageCID = 1;
  h.rscPayloadDataLen = 3;
  CHECK_EQ(rscSendMessage(NULL, NULL), RSC_ERROR_NULL);
  CHECK_EQ(rscSendMessage(&h, NULL), RSC_ERROR_NULL);
}

static void test_send_allocation_failure() {
  uint8_t d[10] = { 0 };
  model.failAllocAt = 1;
  CHECK_EQ(sendData(1, d, sizeof(d)), RSC_ERROR_MEMORY);
  CHECK_EQ(modelQueueCount(model.tx), 0);
}


// D7, D8, D9: replies ========================================================

static void test_d7_nack_error_keeps_sign() {
  CHECK_EQ(rscSendStatusNack(42, (int8_t) RSC_ERROR_REC_CRC, true), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscError_t rc = 0;
  CHECK_EQ(rscWaitForAckNack(42, &rc, 10), RSC_OK);
  CHECK_EQ(rc, RSC_ERROR_REC_CRC);
}

static void test_d8_ack_status_reported() {
  CHECK_EQ(rscSendStatusAck(43, 5), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscError_t rc = -99;
  CHECK_EQ(rscWaitForAckNack(43, &rc, 10), RSC_OK);
  CHECK_EQ(rc, 5);
}

static void test_d8_reply_fields() {
  rscReply_t r;
  CHECK_EQ(rscSendStatusNack(7, (int8_t) RSC_ERROR_CMD_BAD_ARG, true), RSC_OK);
  CHECK_EQ(rscSendStatusNack(8, (int8_t) RSC_ERROR_CMD_NOT_IMP, false), RSC_OK);
  CHECK_EQ(rscSendStatusAck(9, 0), RSC_OK);
  pumpTransmit();
  receiveAll();
  CHECK_EQ(rscWaitForReply(7, &r, 0), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_NACK);
  CHECK_EQ(r.status, RSC_ERROR_CMD_BAD_ARG);
  CHECK(r.resend);
  CHECK_EQ(rscWaitForReply(8, &r, 0), RSC_OK);
  CHECK_EQ(r.status, RSC_ERROR_CMD_NOT_IMP);
  CHECK(!r.resend);
  CHECK_EQ(rscWaitForReply(9, &r, 0), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_ACK);
  CHECK_EQ(r.status, 0);
  CHECK(!r.resend);
  CHECK_EQ(rscWaitForReply(9, &r, 0), RSC_ERROR_TIMEOUT);       // taken once
}

static void test_nack_with_zero_error_is_not_success() {
  CHECK_EQ(rscSendStatusNack(44, 0, false), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscError_t rc = 0;
  CHECK_EQ(rscWaitForAckNack(44, &rc, 10), RSC_OK);
  CHECK_EQ(rc, RSC_ERROR_GENERAL);
}

static void test_d9_wait_keeps_receive_order() {
  const uint8_t a = 1, b = 2;
  CHECK_EQ(sendData(100, &a, 1), RSC_OK);
  CHECK_EQ(rscSendStatusAck(50, 0), RSC_OK);
  CHECK_EQ(sendData(101, &b, 1), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscError_t rc = -1;
  CHECK_EQ(rscWaitForAckNack(50, &rc, 10), RSC_OK);
  rscEnvelope_t e1, e2;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e1, 0), RSC_OK);
  CHECK_EQ(rscPlatformDequeueReceiveData(&e2, 0), RSC_OK);
  CHECK_EQ(e1.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 100);
  CHECK_EQ(e2.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 101);
  CHECK_EQ(modelQueueCount(model.rx), 0);
}

static void test_d9_replies_stay_out_of_receive_queue() {
  CHECK_EQ(rscSendStatusAck(7, 0), RSC_OK);
  CHECK_EQ(rscSendStatusNack(8, -1, false), RSC_OK);
  pumpTransmit();
  receiveAll();
  CHECK_EQ(modelQueueCount(model.rx), 0);
  rscError_t rc;
  CHECK_EQ(rscWaitForAckNack(7, &rc, 10), RSC_OK);
  CHECK_EQ(rscWaitForAckNack(8, &rc, 10), RSC_OK);
}

static void test_d9_wait_delays_between_checks() {
  model.millisStep = 1;               // so that a wait without delays still ends
  rscError_t rc;
  CHECK_EQ(rscWaitForAckNack(77, &rc, 20), RSC_ERROR_TIMEOUT);
  CHECK(model.delayCalls > 0);
}

static void test_wait_timeout() {
  rscReply_t r;
  uint32_t t0 = model.now;
  CHECK_EQ(rscWaitForReply(1, &r, 20), RSC_ERROR_TIMEOUT);
  CHECK(model.now - t0 >= 20 && model.now - t0 <= 22);
  model.delayCalls = 0;
  CHECK_EQ(rscWaitForReply(1, &r, 0), RSC_ERROR_TIMEOUT);
  CHECK_EQ(model.delayCalls, 0);
  CHECK_EQ(rscWaitForReply(1, NULL, 0), RSC_ERROR_NULL);
  CHECK_EQ(rscWaitForAckNack(1, NULL, 0), RSC_ERROR_NULL);
}

static void test_reply_arrives_during_wait() {
  CHECK_EQ(rscSendStatusAck(60, 3), RSC_OK);
  model.onDelay = [] () { pumpTransmit(); receiveAll(); };
  rscError_t rc = 0;
  CHECK_EQ(rscWaitForAckNack(60, &rc, 50), RSC_OK);
  CHECK_EQ(rc, 3);
}

static void test_reply_table_replaces_oldest() {
  for (uint32_t cid = 1; cid <= RSC_REPLY_SLOTS + 1; cid++) CHECK_EQ(rscSendStatusAck(cid, (int8_t) cid), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscReply_t r;
  CHECK_EQ(rscWaitForReply(1, &r, 0), RSC_ERROR_TIMEOUT);      // oldest, replaced
  for (uint32_t cid = 2; cid <= RSC_REPLY_SLOTS + 1; cid++) {
    CHECK_EQ(rscWaitForReply(cid, &r, 0), RSC_OK);
    CHECK_EQ(r.status, (int) cid);
  }
  CHECK_EQ(model.criticalDepth, 0);
}

static void test_reply_same_cid_replaces() {
  CHECK_EQ(rscSendStatusAck(9, 1), RSC_OK);
  CHECK_EQ(rscSendStatusAck(9, 2), RSC_OK);
  pumpTransmit();
  receiveAll();
  rscReply_t r;
  CHECK_EQ(rscWaitForReply(9, &r, 0), RSC_OK);
  CHECK_EQ(r.status, 2);
  CHECK_EQ(rscWaitForReply(9, &r, 0), RSC_ERROR_TIMEOUT);
}

static void test_reply_with_heap_payload_is_freed() {
  std::vector<uint8_t> d(10, 0);
  d[0] = 0xFB;                        // -5
  push(buildFrame(7 + 10, (uint16_t) ~(7 + 10), RSC_MSG_TYPE_ACK, 70, 10, d));
  CHECK_EQ(receiveFrame(), RSC_OK);
  CHECK_EQ(model.allocs, model.frees);
  rscReply_t r;
  CHECK_EQ(rscWaitForReply(70, &r, 0), RSC_OK);
  CHECK_EQ(r.status, -5);
}


// Reliable send ==============================================================

enum { RESPOND_ACK, RESPOND_NACK_RESEND, RESPOND_NACK_KEEP, RESPOND_NONE };
static std::vector<int> script;
static int dataSeen;

// The peer: answers each data message by the script (the last entry repeats)
static void responder() {
  pumpTransmit();
  receiveAll();
  rscEnvelope_t e;
  while ( rscPlatformDequeueReceiveData(&e, 0) == RSC_OK ) {
    uint32_t cid = e.rscEnvelopePayload.rscPayloadHeader.rscMessageCID;
    freePayload(e);
    int action = script.empty() ? RESPOND_NONE : script[(size_t) dataSeen < script.size() ? (size_t) dataSeen : script.size() - 1];
    dataSeen++;
    if ( action == RESPOND_ACK ) rscSendStatusAck(cid, 0);
    if ( action == RESPOND_NACK_RESEND ) rscSendStatusNack(cid, (int8_t) RSC_ERROR_REC_CRC, true);
    if ( action == RESPOND_NACK_KEEP ) rscSendStatusNack(cid, (int8_t) RSC_ERROR_CMD_BAD_ARG, false);
    pumpTransmit();
    receiveAll();
  }
}

static rscError_t reliable(uint8_t attempts, rscReply_t* r) {
  const uint8_t d[] = { 9, 8, 7, 6, 5, 4, 3, 2 };
  rscMessageHeader_t h;
  h.rscMessageType = RSC_MSG_TYPE_DATA;
  h.rscMessageCID = rscGetNextCID();
  h.rscPayloadDataLen = sizeof(d);
  dataSeen = 0;
  model.onDelay = responder;
  return rscSendMessageReliable(&h, d, attempts, 30, r);
}

static void test_reliable_ack_first() {
  rscReply_t r;
  script = { RESPOND_ACK };
  CHECK_EQ(reliable(3, &r), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_ACK);
  CHECK_EQ(dataSeen, 1);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_reliable_resend_then_ack() {
  rscReply_t r;
  script = { RESPOND_NACK_RESEND, RESPOND_ACK };
  CHECK_EQ(reliable(3, &r), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_ACK);
  CHECK_EQ(dataSeen, 2);
}

static void test_reliable_nack_keep_stops() {
  rscReply_t r;
  script = { RESPOND_NACK_KEEP };
  CHECK_EQ(reliable(3, &r), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_NACK);
  CHECK_EQ(r.status, RSC_ERROR_CMD_BAD_ARG);
  CHECK(!r.resend);
  CHECK_EQ(dataSeen, 1);
}

static void test_reliable_resend_on_last_attempt() {
  rscReply_t r;
  script = { RESPOND_NACK_RESEND };
  CHECK_EQ(reliable(2, &r), RSC_OK);
  CHECK_EQ(r.type, RSC_MSG_TYPE_NACK);
  CHECK(r.resend);
  CHECK_EQ(dataSeen, 2);
}

static void test_reliable_timeout() {
  rscReply_t r;
  script = { RESPOND_NONE };
  CHECK_EQ(reliable(3, &r), RSC_ERROR_TIMEOUT);
  CHECK_EQ(dataSeen, 3);
  dataSeen = 0;
  CHECK_EQ(reliable(0, &r), RSC_ERROR_TIMEOUT);          // 0 attempts count as 1
  CHECK_EQ(dataSeen, 1);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_reliable_send_error() {
  rscReply_t r;
  script = { RESPOND_ACK };
  model.tx.capacity = 0;
  CHECK_EQ(reliable(3, &r), RSC_ERROR_TRX_Q_FULL);
  CHECK_EQ(rscSendMessageReliable(NULL, NULL, 1, 1, &r), RSC_ERROR_NULL);
}


// Receive error paths (blocking reader) ======================================

static void test_receive_partial_frame_frees() {
  std::vector<uint8_t> f = validFrame(1, 30);
  f.resize(f.size() - 10);
  push(f);
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_TIMEOUT);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_receive_missing_crc_frees() {
  std::vector<uint8_t> f = validFrame(1, 30);
  f.resize(f.size() - 1);
  push(f);
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_TIMEOUT);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_receive_bad_crc() {
  push(validFrame(1, 30, false));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_CRC);
  push(validFrame(2, 2, false));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_CRC);
  CHECK_EQ(modelQueueCount(model.rx), 0);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_receive_out_of_memory() {
  model.failAllocAt = 1;
  push(validFrame(1, 30));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_OOM);
  receiveAll();                       // the stream recovers
  push(validFrame(2, 30));
  CHECK_EQ(receiveFrame(), RSC_OK);
  CHECK_EQ(modelQueueCount(model.rx), 1);
}

static void test_receive_queue_full_frees() {
  model.rx.capacity = 0;
  push(validFrame(1, 30));
  CHECK_EQ(receiveFrame(), RSC_ERROR_REC_Q_FULL);
  CHECK_EQ(model.allocs, model.frees);
}


// Transmit ===================================================================

static void test_transmit_poll() {
  const uint8_t d[] = { 1, 2, 3, 4, 5, 6, 7 };
  CHECK_EQ(rscTransmitPoll(), RSC_ERROR_TRX_Q_EMPTY);
  CHECK_EQ(sendData(1, d, sizeof(d)), RSC_OK);
  CHECK_EQ(rscTransmitPoll(), RSC_OK);
  CHECK_EQ(rscTransmitPoll(), RSC_ERROR_TRX_Q_EMPTY);
  CHECK_EQ(model.allocs, model.frees);
  CHECK_EQ(receiveFrame(), RSC_OK);
}

static void test_transmit_send_failure_frees() {
  const uint8_t d[] = { 1, 2, 3, 4, 5, 6, 7 };
  for (int call = 1; call <= 3; call++) {
    for (int shortSend = 0; shortSend <= 1; shortSend++) {
      modelReset();
      model.failSendAt = call;
      model.shortSend = shortSend != 0;
      CHECK_EQ(sendData(1, d, sizeof(d)), RSC_OK);
      CHECK_EQ(rscTransmitPoll(), RSC_ERROR_GENERAL);
      CHECK_EQ(model.allocs, model.frees);
    }
  }
}


// Non-blocking receive =======================================================

static void test_poll_byte_by_byte() {
  std::vector<uint8_t> f = validFrame(31, 25);
  for (size_t i = 0; i < f.size(); i++) {
    modelWirePush(&f[i], 1);
    CHECK_EQ(rscReceivePoll(), RSC_OK);
    model.now += 5;
  }
  rscEnvelope_t e;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e, 0), RSC_OK);
  CHECK_EQ(e.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 31);
  CHECK_EQ(payloadOf(e)[24], 24);
  freePayload(e);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_poll_several_frames_and_junk() {
  const uint8_t junk[] = { 0x00, RSC_ENVELOPE_BYTE1, RSC_ENVELOPE_BYTE1 };
  push(validFrame(1, 2));
  modelWirePush(junk, sizeof(junk));
  push(validFrame(2, 30));
  CHECK_EQ(rscSendStatusAck(3, 0), RSC_OK);
  pumpTransmit();
  CHECK_EQ(rscReceivePoll(), RSC_OK);
  CHECK_EQ(modelQueueCount(model.rx), 2);
  rscReply_t r;
  CHECK_EQ(rscWaitForReply(3, &r, 0), RSC_OK);
  rscEnvelope_t e;
  while ( rscPlatformDequeueReceiveData(&e, 0) == RSC_OK ) freePayload(e);
  CHECK_EQ(model.allocs, model.frees);
}

static void test_poll_gap_drops_frame() {
  std::vector<uint8_t> f = validFrame(1, 30);
  modelWirePush(f.data(), 20);
  CHECK_EQ(rscReceivePoll(), RSC_OK);
  model.now += RSC_DEFAULT_TOUT + 1;
  CHECK_EQ(rscReceivePoll(), RSC_ERROR_REC_TIMEOUT);
  CHECK_EQ(model.allocs, model.frees);
  push(validFrame(2, 30));
  CHECK_EQ(rscReceivePoll(), RSC_OK);
  CHECK_EQ(modelQueueCount(model.rx), 1);
  rscEnvelope_t e;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e, 0), RSC_OK);
  CHECK_EQ(e.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 2);
  freePayload(e);
}

static void test_poll_rejects_bad_frames() {
  std::vector<uint8_t> bad = validFrame(1, 2);
  bad[4] = bad[5] = 0;
  push(bad);
  CHECK_EQ(rscReceivePoll(), RSC_ERROR_REC_BAD_LEN);
  std::vector<uint8_t> d(64, 0x5A);
  push(buildFrame(7 + 64, (uint16_t) ~(7 + 64), RSC_MSG_TYPE_DATA, 1, 2, d));
  CHECK_EQ(rscReceivePoll(), RSC_ERROR_REC_BAD_LEN);
  push(validFrame(1, 30, false));
  CHECK_EQ(rscReceivePoll(), RSC_ERROR_REC_CRC);
  model.failAllocAt = model.allocCalls + 1;
  push(validFrame(1, 30));
  CHECK_EQ(rscReceivePoll(), RSC_ERROR_REC_OOM);
  push(validFrame(5, 30));
  CHECK_EQ(rscReceivePoll(), RSC_OK);
  CHECK_EQ(modelQueueCount(model.rx), 1);
  rscEnvelope_t e;
  CHECK_EQ(rscPlatformDequeueReceiveData(&e, 0), RSC_OK);
  CHECK_EQ(e.rscEnvelopePayload.rscPayloadHeader.rscMessageCID, 5);
  freePayload(e);
  CHECK_EQ(model.allocs, model.frees);
}


// Other ======================================================================

static void test_next_cid() {
  uint32_t a = rscGetNextCID();
  CHECK_EQ(rscGetNextCID(), a + 1);
  CHECK_EQ(model.criticalDepth, 0);
  CHECK_EQ(model.criticalMax, 1);
}

static void test_version_consistency() {
  char v[16];
  snprintf(v, sizeof(v), "%d.%d.%d", RSC_VERSION_MAJOR, RSC_VERSION_MINOR, RSC_VERSION_RELEASE);
  CHECK(strcmp(v, RSC_VERSION_STRING) == 0);
  CHECK_EQ(RSC_VERSION, RSC_VERSION_MAJOR * 10000 + RSC_VERSION_MINOR * 100 + RSC_VERSION_RELEASE);

  std::string ver = RSC_VERSION_STRING;
  std::string json = readFile("library.json");
  std::string props = readFile("library.properties");
  std::string readme = readFile("README.md");
  CHECK(json.find("\"version\": \"" + ver + "\"") != std::string::npos);
  CHECK(props.find("\nversion=" + ver + "\n") != std::string::npos);
  // the version line near the top, and the version history entry
  CHECK(readme.substr(0, 400).find("Version " + ver) != std::string::npos);
  size_t hist = readme.find("## Version history");
  CHECK(hist != std::string::npos && readme.find("- **" + ver + "**", hist) != std::string::npos);
}

static void test_legacy_names() {
#ifndef RSC_NO_LEGACY
  rscEnvelope_t qd;
  CHECK_EQ(RSC_MSG_TYPE_RES, RSC_MSG_TYPE_ACK);
  CHECK_EQ(sizeof(rscRelayStatus_t), 9);
  CHECK_EQ(rscSendStatusAck(1, 0), RSC_OK);
  CHECK(rscTransmitLoopReady(&qd, 0));
  CHECK_EQ(rscTrasmitLoopSend(&qd), RSC_OK);
  uint8_t b = 0;
  CHECK_EQ(rscPlatfromSendBytes(&b, 1), 1);
#else
#if defined(rscTrasmitLoopSend) || defined(RSC_MSG_TYPE_RES)
#error "RSC_NO_LEGACY must hide the 1.0.0 names"
#endif
#endif
}


// Runner =====================================================================

struct TestCase {
  const char* name;
  void (*fn)();
};

#define T(f) { #f, f }
static const TestCase tests[] = {
  T(test_crc_check_value),
  T(test_wire_golden_nack),
  T(test_wire_golden_data),
  T(test_wire_golden_ack),
  T(test_roundtrip_sizes),
  T(test_resync_on_repeated_start_byte),
  T(test_d3_inverse_length_checked),
  T(test_d4_length_above_embedded_data),
  T(test_d4_length_above_heap_data),
  T(test_d4_length_below_data),
  T(test_d5_length_below_header),
  T(test_padded_length_accepted),
  T(test_d6_no_leak_when_transmit_queue_full),
  T(test_d10_payload_above_32767),
  T(test_d11_data_length_limit),
  T(test_max_data_length_roundtrip),
  T(test_send_null_arguments),
  T(test_send_allocation_failure),
  T(test_d7_nack_error_keeps_sign),
  T(test_d8_ack_status_reported),
  T(test_d8_reply_fields),
  T(test_nack_with_zero_error_is_not_success),
  T(test_d9_wait_keeps_receive_order),
  T(test_d9_replies_stay_out_of_receive_queue),
  T(test_d9_wait_delays_between_checks),
  T(test_wait_timeout),
  T(test_reply_arrives_during_wait),
  T(test_reply_table_replaces_oldest),
  T(test_reply_same_cid_replaces),
  T(test_reply_with_heap_payload_is_freed),
  T(test_reliable_ack_first),
  T(test_reliable_resend_then_ack),
  T(test_reliable_nack_keep_stops),
  T(test_reliable_resend_on_last_attempt),
  T(test_reliable_timeout),
  T(test_reliable_send_error),
  T(test_receive_partial_frame_frees),
  T(test_receive_missing_crc_frees),
  T(test_receive_bad_crc),
  T(test_receive_out_of_memory),
  T(test_receive_queue_full_frees),
  T(test_transmit_poll),
  T(test_transmit_send_failure_frees),
  T(test_poll_byte_by_byte),
  T(test_poll_several_frames_and_junk),
  T(test_poll_gap_drops_frame),
  T(test_poll_rejects_bad_frames),
  T(test_next_cid),
  T(test_version_consistency),
  T(test_legacy_names),
};

// Empty the reply table and the poll parser between tests: RSC_REPLY_SLOTS fresh
// replies replace whatever an earlier test left, then they are taken
static void drainLibraryState() {
  rscReply_t r;
  for (uint32_t i = 0; i < RSC_REPLY_SLOTS; i++) rscSendStatusAck(0xFFFF0000u + i, 0);
  pumpTransmit();
  receiveAll();
  for (uint32_t i = 0; i < RSC_REPLY_SLOTS; i++) rscWaitForReply(0xFFFF0000u + i, &r, 0);
  model.now += 10 * RSC_DEFAULT_TOUT;
  rscReceivePoll();
}

int main(int argc, char** argv) {
  int run = 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  atexit(modelReset);                 // free what the last test left in the queues
  for (const TestCase& t : tests) {
    if ( argc > 1 && strcmp(argv[1], t.name) != 0 ) continue;
    modelReset();
    drainLibraryState();
    modelReset();
    int before = failures;
    t.fn();
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    run++;
  }
  printf("%d tests, %d failed checks\n", run, failures);
  return ( failures == 0 && run > 0 ) ? 0 : 1;
}
