#pragma once

#include <Arduino.h>

// Platform layer of ReliableSerialComms for a superloop on arduino-esp32: one
// HardwareSerial port and two static queues of RSC_MSG_Q_DEPTH envelopes. Nothing waits:
// the queue timeouts are ignored, and rscPlatformReceiveReady() returns at once.
void rscPlatformBegin(HardwareSerial& serial);
