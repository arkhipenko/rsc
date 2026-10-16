/*
  ReliableSerialComms: loopback example for ESP32 (arduino-esp32 2.x and 3.x)

  One UART talks to itself. Wire its TX pin to its RX pin (UART_TX_PIN and UART_RX_PIN below).
  Four FreeRTOS tasks share the work:
    - receive:   rscReceiveLoop() reads frames from the UART into the receive queue
                 (data) and the reply table (ACK and NACK)
    - transmit:  rscTransmitLoop() writes frames from the transmit queue to the UART
    - requester: sends one data message per second with rscSendMessageReliable()
                 (3 attempts, 500 ms each) and prints the reply
    - responder: takes data messages from the receive queue, prints them and sends an ACK

  Expected output on the Serial monitor, once per second:
    received CID 0: hello 0
    CID 0: ACK, status 0
  Without the wire, the requester prints "no reply" after 3 attempts.

  Two boards can run the same sketch: TX of each to RX of the other, and a common ground.
  Each board then answers the other's messages.
*/

#include <rsc_framework.h>
#include "rsc_platform_esp32.h"

// UART pins (GPIO 4 and 5 on ESP32-C3, GPIO 16 and 17 on the other chips)
#if CONFIG_IDF_TARGET_ESP32C3
static const int8_t UART_RX_PIN = 4;
static const int8_t UART_TX_PIN = 5;
#else
static const int8_t UART_RX_PIN = 16;
static const int8_t UART_TX_PIN = 17;
#endif
#define UART_BAUD    115200

static void receiveTask(void* arg) {
  (void) arg;
  rscReceiveLoop();
}

static void transmitTask(void* arg) {
  (void) arg;
  rscTransmitLoop();
}

static void requesterTask(void* arg) {
  (void) arg;
  uint32_t counter = 0;

  for (;;) {
    char text[32];
    int len = snprintf(text, sizeof(text), "hello %lu", (unsigned long) counter++);

    rscMessageHeader_t h;
    h.rscMessageType = RSC_MSG_TYPE_DATA;
    h.rscMessageCID = rscGetNextCID();
    h.rscPayloadDataLen = (uint16_t) len;

    rscReply_t reply;
    rscError_t rc = rscSendMessageReliable(&h, (const uint8_t*) text, 3, 500, &reply);
    unsigned long cid = (unsigned long) h.rscMessageCID;
    if ( rc == RSC_OK && reply.type == RSC_MSG_TYPE_ACK ) {
      Serial.printf("CID %lu: ACK, status %d\n", cid, reply.status);
    }
    else if ( rc == RSC_OK ) {
      Serial.printf("CID %lu: NACK, error %d\n", cid, reply.status);
    }
    else {
      Serial.printf("CID %lu: no reply (error %d). Is TX wired to RX?\n", cid, rc);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void responderTask(void* arg) {
  (void) arg;
  rscEnvelope_t e;

  for (;;) {
    if ( rscPlatformDequeueReceiveData(&e, RSC_BLOCKING) != RSC_OK ) continue;

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

void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("ReliableSerialComms %s loopback: wire GPIO %d to GPIO %d\n", RSC_VERSION_STRING, UART_TX_PIN, UART_RX_PIN);

  if ( !rscPlatformBegin(Serial1) ) {
    Serial.println("Queue allocation failed");
    return;
  }
  if ( xTaskCreate(receiveTask, "rscRx", 4096, NULL, 3, NULL) != pdPASS ||
       xTaskCreate(transmitTask, "rscTx", 4096, NULL, 3, NULL) != pdPASS ||
       xTaskCreate(responderTask, "responder", 4096, NULL, 2, NULL) != pdPASS ||
       xTaskCreate(requesterTask, "requester", 4096, NULL, 1, NULL) != pdPASS ) {
    Serial.println("Task creation failed");
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
