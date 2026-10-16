/*
  ReliableSerialComms: polling example for ESP32 (arduino-esp32 2.x and 3.x)

  The exchange of RSC_Loopback without tasks: loop() drives everything and never waits.
    - rscReceivePoll() parses the bytes that have arrived
    - rscTransmitPoll() writes the queued frames
    - received data messages are printed and answered with an ACK
    - one data message per second; rscWaitForReply(cid, &reply, 0) checks for its reply,
      and the message is sent again after 500 ms without a reply (3 attempts)

  Wire the TX pin to the RX pin of the same UART (UART_TX_PIN and UART_RX_PIN below).
  Expected output on the Serial monitor, once per second:
    received CID 0: hello 0
    CID 0: ACK, status 0
  Without the wire, "no reply" is printed after 3 attempts.
*/

#include <rsc_framework.h>
#include "rsc_platform_poll.h"

// UART pins (GPIO 4 and 5 on ESP32-C3, GPIO 16 and 17 on the other chips)
#if CONFIG_IDF_TARGET_ESP32C3
static const int8_t UART_RX_PIN = 4;
static const int8_t UART_TX_PIN = 5;
#else
static const int8_t UART_RX_PIN = 16;
static const int8_t UART_TX_PIN = 17;
#endif
#define UART_BAUD          115200
#define REPLY_TIMEOUT_MS  500
#define ATTEMPTS          3

static rscMessageHeader_t request;        // the message waiting for its reply
static char               requestText[32];
static bool               waiting = false;
static uint8_t            attempts = 0;
static uint32_t           sentAt = 0;
static uint32_t           nextRequestAt = 0;
static uint32_t           counter = 0;

static void sendRequest() {
  attempts++;
  sentAt = millis();
  rscError_t rc = rscSendMessage(&request, (const uint8_t*) requestText);
  if ( rc != RSC_OK ) Serial.printf("CID %lu: send failed, error %d\n", (unsigned long) request.rscMessageCID, rc);
}

static void answerReceivedData() {
  rscEnvelope_t e;

  while ( rscPlatformDequeueReceiveData(&e, 0) == RSC_OK ) {
    rscMessageHeader_t* h = &e.rscEnvelopePayload.rscPayloadHeader;
    bool onHeap = h->rscPayloadDataLen > RSC_EMB_DATA_LEN;
    const uint8_t* data = onHeap ? e.rscEnvelopePayload.rscPayloadData.ptr : e.rscEnvelopePayload.rscPayloadData.data;
    Serial.printf("received CID %lu: %.*s\n", (unsigned long) h->rscMessageCID, (int) h->rscPayloadDataLen, (const char*) data);

    rscError_t rc = rscSendStatusAck(h->rscMessageCID, 0);
    if ( rc != RSC_OK ) Serial.printf("ACK not queued: error %d\n", rc);

    // the receiver owns a payload longer than RSC_EMB_DATA_LEN bytes
    if ( onHeap ) rscPlatformFree(e.rscEnvelopePayload.rscPayloadData.ptr);
  }
}

static void checkRequest() {
  unsigned long cid = (unsigned long) request.rscMessageCID;
  rscReply_t reply;

  if ( rscWaitForReply(request.rscMessageCID, &reply, 0) == RSC_OK ) {
    if ( reply.type == RSC_MSG_TYPE_NACK && reply.resend && attempts < ATTEMPTS ) {
      sendRequest();
      return;
    }
    if ( reply.type == RSC_MSG_TYPE_ACK ) Serial.printf("CID %lu: ACK, status %d\n", cid, reply.status);
    else Serial.printf("CID %lu: NACK, error %d\n", cid, reply.status);
    waiting = false;
    return;
  }
  if ( millis() - sentAt >= REPLY_TIMEOUT_MS ) {
    if ( attempts < ATTEMPTS ) {
      sendRequest();
      return;
    }
    Serial.printf("CID %lu: no reply. Is TX wired to RX?\n", cid);
    waiting = false;
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("ReliableSerialComms %s polling: wire GPIO %d to GPIO %d\n", RSC_VERSION_STRING, UART_TX_PIN, UART_RX_PIN);
  rscPlatformBegin(Serial1);
}

void loop() {
  rscError_t rc = rscReceivePoll();
  if ( rc != RSC_OK ) Serial.printf("frame dropped: error %d\n", rc);

  answerReceivedData();

  if ( waiting ) {
    checkRequest();
  }
  else if ( (int32_t) (millis() - nextRequestAt) >= 0 ) {
    nextRequestAt = millis() + 1000;
    int len = snprintf(requestText, sizeof(requestText), "hello %lu", (unsigned long) counter++);
    request.rscMessageType = RSC_MSG_TYPE_DATA;
    request.rscMessageCID = rscGetNextCID();
    request.rscPayloadDataLen = (uint16_t) len;
    attempts = 0;
    waiting = true;
    sendRequest();
  }

  while ( rscTransmitPoll() == RSC_OK ) {}
}
