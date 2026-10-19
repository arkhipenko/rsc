/*
  RSC_TwoLinks: two ReliableSerialComms links on one ESP32, joined by two wires, each link
  served by its own FreeRTOS task. A third task sends a request every second on link A and
  blocks until link B has answered it.

  Wiring (ESP32 with three UARTs):
    GPIO 19 (Serial1 TX) -> GPIO 16 (Serial2 RX)
    GPIO 17 (Serial2 TX) -> GPIO 18 (Serial1 RX)
  Results print on Serial (USB) at 115200.
*/

#include <ReliableSerialComms.h>

#if !defined(ESP32)
#error "This example needs an ESP32 with three UARTs (Serial2)"
#endif

const uint32_t BAUD = 921600;
const int A_RX = 18;
const int A_TX = 19;
const int B_RX = 16;
const int B_TX = 17;

const uint8_t CMD_ECHO = 0x01;

// One link with its task, its lock and its stream
struct LinkTask {
  rscLink_t         link;
  HardwareSerial*   serial;
  SemaphoreHandle_t mutex;
  TaskHandle_t      task;
  const char*       name;
};

static LinkTask linkA;
static LinkTask linkB;
static SemaphoreHandle_t answered;    // given by the response callback of link A
static rscResponse_t lastResponse;
static uint8_t lastData[16];

static void opLock(void* ctx) {
  xSemaphoreTake(((LinkTask*) ctx)->mutex, portMAX_DELAY);
}

static void opUnlock(void* ctx) {
  xSemaphoreGive(((LinkTask*) ctx)->mutex);
}

static void opWake(void* ctx) {
  LinkTask* t = (LinkTask*) ctx;
  if ( t->task ) xTaskNotifyGive(t->task);
}

static int opWrite(void* ctx, const uint8_t* data, size_t length) {
  return (int) ((LinkTask*) ctx)->serial->write(data, length);
}

static uint32_t opMillis(void* ctx) {
  (void) ctx;
  return millis();
}

static uint32_t opRandom(void* ctx) {
  return rscStreamRandom(ctx);          // Arduino random(); hardware-backed on arduino-esp32 3.x
}

// Link B answers ECHO with the same data
static bool onMessageB(rscLink_t* link, const rscMessage_t* m, void* user) {
  (void) user;
  if ( m->tag != 0 && m->type == CMD_ECHO ) return rscAck(link, m->tag, m->data, m->length) != RSC_ERROR_QUEUE_FULL;
  return true;
}

static void onEvent(rscLink_t* link, uint8_t event, void* user) {
  (void) link;
  const char* what = event == RSC_EVENT_UP ? "up" : event == RSC_EVENT_DOWN ? "down" : "peer restarted";
  Serial.printf("link %s: %s\n", ((LinkTask*) user)->name, what);
}

// Runs in the task of link A: copy the outcome, then wake the requester
static void onResponse(rscLink_t* link, const rscResponse_t* rsp, void* user) {
  (void) link;
  (void) user;
  if ( rsp->event == RSC_RESPONSE_PROGRESS ) return;
  lastResponse = *rsp;
  lastResponse.length = (uint16_t) ( rsp->length < sizeof(lastData) ? rsp->length : sizeof(lastData) );
  memcpy(lastData, rsp->data, lastResponse.length);
  lastResponse.data = lastData;
  xSemaphoreGive(answered);
}

// Feed received bytes and run the link; sleep until the next deadline, new bytes or a wake
static void linkTask(void* arg) {
  LinkTask* t = (LinkTask*) arg;
  for (;;) {
    uint32_t wait = rscStreamPoll(&t->link, *t->serial);
    if ( wait > 2 ) wait = 2;         // the UART driver has no wake-up for received bytes here
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait) > 0 ? pdMS_TO_TICKS(wait) : 1);
  }
}

static void requesterTask(void* arg) {
  (void) arg;
  uint32_t n = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if ( !rscIsUp(&linkA.link) ) continue;
    uint8_t data[4] = { (uint8_t) n, (uint8_t) (n >> 8), (uint8_t) (n >> 16), (uint8_t) (n >> 24) };
    uint8_t tag = 0;
    uint32_t start = millis();
    if ( rscRequest(&linkA.link, CMD_ECHO, data, sizeof(data), 500, onResponse, NULL, &tag) != RSC_OK ) continue;
    xSemaphoreTake(answered, portMAX_DELAY);
    if ( lastResponse.event == RSC_RESPONSE_ACK && lastResponse.length == 4 && memcmp(lastData, data, 4) == 0 ) {
      Serial.printf("request %u (tag %u): echoed in %u ms\n", (unsigned) n, tag, (unsigned) (millis() - start));
    }
    else {
      Serial.printf("request %u (tag %u): event %u\n", (unsigned) n, tag, lastResponse.event);
    }
    n++;
  }
}

static void startLink(LinkTask& t, const char* name, HardwareSerial& serial, int rx, int tx, rscMessageCb_t onMessage) {
  t.name = name;
  t.serial = &serial;
  t.mutex = xSemaphoreCreateMutex();
  t.task = NULL;
  serial.begin(BAUD, SERIAL_8N1, rx, tx);

  rscOps_t ops;
  memset(&ops, 0, sizeof(ops));
  ops.write = opWrite;
  ops.millis = opMillis;
  ops.random = opRandom;
  ops.lock = opLock;
  ops.unlock = opUnlock;
  ops.wake = opWake;
  ops.ctx = &t;

  rscConfig_t cfg;
  rscConfigDefaults(&cfg);
  cfg.baud = BAUD;
  cfg.onMessage = onMessage;
  cfg.onEvent = onEvent;
  cfg.user = &t;
  rscInit(&t.link, &cfg, &ops);
  xTaskCreate(linkTask, name, 4096, &t, 5, &t.task);
}

void setup() {
  Serial.begin(115200);
  answered = xSemaphoreCreateBinary();
  startLink(linkA, "A", Serial1, A_RX, A_TX, NULL);
  startLink(linkB, "B", Serial2, B_RX, B_TX, onMessageB);
  xTaskCreate(requesterTask, "requester", 4096, NULL, 4, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
