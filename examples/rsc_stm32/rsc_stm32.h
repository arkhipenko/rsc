#pragma once

// ReliableSerialComms on STM32 HAL with FreeRTOS: one link over USART1, served by one task.
// This folder is a fragment of an STM32CubeIDE project, not a complete project.
// See README.md for what the project has to supply.

#include <stdint.h>
#include "rsc.h"

// The link. Other tasks queue messages on it (rscSend, rscRequest, rscAck, ...).
extern rscLink_t rscStm32Link;

// Bytes the UART interrupt could not store because rxRingBuffer was full
extern volatile uint32_t rscStm32RxDropped;

// Set up the link before the scheduler starts. onMessage answers requests from the peer.
void rscStm32Init(rscMessageCb_t onMessage, rscEventCb_t onEvent);

// Entry function of the task that runs the link (create it in CubeMX or with xTaskCreate)
void startRscTask(void* argument);
