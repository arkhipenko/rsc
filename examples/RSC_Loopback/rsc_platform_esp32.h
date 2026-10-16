#pragma once

#include <Arduino.h>

// Platform layer of ReliableSerialComms for arduino-esp32: one HardwareSerial port,
// FreeRTOS queues of RSC_MSG_Q_DEPTH envelopes, a spinlock for the critical section.
// Call once before the tasks start. Returns false if a queue cannot be allocated.
bool rscPlatformBegin(HardwareSerial& serial);
