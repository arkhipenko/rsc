/*
  RSC_Host: a ReliableSerialComms responder on the USB serial port of any Arduino board
  (Uno, Mega, ESP32, ...). Run python/examples/host_demo.py on the computer to talk to it.

  Commands (requests from the host):
    0x01 ECHO   ACK with the request data
    0x02 ADD    two int32 (little-endian); response type 0x02 with their sum
    0x03 SLOW   PROGRESS at once, ACK one second later
    0x04 INFO   response type 0x04 with a text line
    other       NACK RSC_ERROR_CMD_NOT_IMP
  Events (to the host):
    0x10 UPTIME every 5 seconds, milliseconds since start (uint32, little-endian)

  Serial carries the protocol, so the sketch prints nothing.
*/

#include <ReliableSerialComms.h>

const uint32_t BAUD = 115200;

const uint8_t CMD_ECHO = 0x01;
const uint8_t CMD_ADD = 0x02;
const uint8_t CMD_SLOW = 0x03;
const uint8_t CMD_INFO = 0x04;
const uint8_t EVT_UPTIME = 0x10;

static rscLink_t hostLink;
static bool refused = false;          // a request was refused because the queue was full
static uint8_t slowTag = 0;           // the SLOW request in progress
static uint32_t slowDoneAt = 0;
static uint32_t lastUptime = 0;

static int32_t get32(const uint8_t* p) {
  return (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24));
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}

// Answer a request. Returning false refuses it when the answer does not fit the transmit
// queue: the host pauses, and loop() resumes the link once the queue has room.
static bool onMessage(rscLink_t* link, const rscMessage_t* m, void* user) {
  (void) user;
  rscError_t rc = RSC_OK;
  uint8_t out[24];

  if ( m->tag == 0 ) return true;     // events from the host: none are used here

  switch ( m->type ) {
    case CMD_ECHO:
      rc = rscAck(link, m->tag, m->data, m->length);
      break;

    case CMD_ADD:
      if ( m->length != 8 ) {
        rc = rscNack(link, m->tag, RSC_ERROR_CMD_BAD_ARG, NULL, 0);
        break;
      }
      put32(out, (uint32_t) (get32(m->data) + get32(m->data + 4)));
      rc = rscRespond(link, m->tag, CMD_ADD, out, 4);
      break;

    case CMD_SLOW:
      if ( slowTag != 0 ) {
        rc = rscNack(link, m->tag, RSC_ERROR_CMD_BUSY, NULL, 0);
        break;
      }
      rc = rscProgress(link, m->tag, NULL, 0);
      if ( rc == RSC_OK ) {
        slowTag = m->tag;
        slowDoneAt = millis() + 1000;
      }
      break;

    case CMD_INFO: {
      int n = snprintf((char*) out, sizeof(out), "RSC %s MTU %u", RSC_VERSION_STRING, (unsigned) rscMtu(link));
      rc = rscRespond(link, m->tag, CMD_INFO, out, (uint16_t) n);
      break;
    }

    default:
      rc = rscNack(link, m->tag, RSC_ERROR_CMD_NOT_IMP, NULL, 0);
      break;
  }

  if ( rc == RSC_ERROR_QUEUE_FULL ) {
    refused = true;
    return false;
  }
  return true;
}

void setup() {
#if defined(ARDUINO_ARCH_AVR)
  randomSeed(analogRead(A0));         // a different session nonce on every start
#endif
  Serial.begin(BAUD);

  rscConfig_t cfg;
  rscConfigDefaults(&cfg);
  cfg.baud = BAUD;
  cfg.onMessage = onMessage;
  rscOps_t ops = rscStreamOps(Serial);
  rscInit(&hostLink, &cfg, &ops);
}

void loop() {
  rscStreamPoll(&hostLink, Serial);

  // the SLOW command finishes; try again later if the queue is full
  if ( slowTag != 0 && (int32_t) (millis() - slowDoneAt) >= 0 ) {
    if ( rscAck(&hostLink, slowTag, (const uint8_t*) "done", 4) == RSC_OK ) slowTag = 0;
  }

  if ( refused && rscQueued(&hostLink) < RSC_TX_SLOTS ) {
    refused = false;
    rscResume(&hostLink);
  }

  if ( rscIsUp(&hostLink) && millis() - lastUptime >= 5000 ) {
    uint8_t up[4];
    put32(up, millis());
    if ( rscSend(&hostLink, EVT_UPTIME, up, 4) == RSC_OK ) lastUptime = millis();
  }
}
