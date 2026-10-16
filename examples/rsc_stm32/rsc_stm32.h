#pragma once

// ReliableSerialComms platform layer for STM32 HAL with FreeRTOS (CMSIS-RTOS v1 queues).
// This folder is a fragment of an STM32CubeIDE project, not a complete project.
// See README.md for what the project has to supply.

#include <stdint.h>
#include "rsc_framework.h"

// Ticks the two RSC tasks wait before they start
#ifndef TASK_INITIAL_DELAY
#define TASK_INITIAL_DELAY  100
#endif

// Element of the queues qTxHandle and qRxHandle: create both with this item size
typedef struct {
  rscEnvelope_t rscEnvelope;
  uint32_t      timestamp;      // rscPlatformMillis() when queued
} rscQueueEntry_t;

// Bytes the UART interrupt could not store because rxRingBuffer was full
extern volatile uint32_t rscStm32RxDropped;

// Task functions for the CubeMX task definitions
void startDataRx(void *argument);
void startDataTx(void *argument);
