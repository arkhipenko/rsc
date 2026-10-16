# STM32 platform layer (STM32CubeIDE fragment)

This folder holds the ReliableSerialComms platform layer for STM32 HAL with FreeRTOS (CMSIS-RTOS v1 API). It is a fragment of an STM32CubeIDE project. It does not build on its own, and CI does not compile it.

## Files

- `rsc_framework_stm32.c`: the platform functions, the UART receive interrupt callback, and the two task entry functions.
- `rngbuffer_stm32.c`: the lock hooks of RngBuffer 2.x (a FreeRTOS critical section).
- `rsc_stm32.h`: the queue element `rscQueueEntry_t`, `TASK_INITIAL_DELAY`, and the dropped-byte counter.

## What the project supplies

1. The library sources: `src/rsc_framework.c`, `src/rsc_crc.c` and the headers of this repository.
2. RngBuffer 2.0.1 (https://github.com/arkhipenko/RngBuffer, tag v2.0.1): `RngBuffer.c` and `RngBuffer.h`.
3. USART1 with its global interrupt enabled, and the handle `UART_HandleTypeDef huart1` (CubeMX generates it).
4. Two queues, `osMessageQId qTxHandle` and `qRxHandle`, with item size `sizeof(rscQueueEntry_t)` and length `RSC_MSG_Q_DEPTH`. Do not define `rscQueueEntry_t` anywhere else.
5. Two tasks with the entry functions `startDataRx` and `startDataTx`.
6. `RngBuffer rxRingBuffer`, initialized with `rng_buffer_init()` over a static byte array before the scheduler starts.
7. One call of `HAL_UART_Receive_IT(&huart1, &serial_byte_received, 1)` after the ring buffer is initialized, to arm the receive interrupt (`extern uint8_t serial_byte_received;`).

## Requirements

- `configTICK_RATE_HZ` must divide 1000 (for example 1000 or 100). `rscPlatformMillis()` uses `portTICK_PERIOD_MS`, which is an integer.
- *** IMPORTANT *** The USART1 interrupt priority must be numerically equal to or greater than `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`. The ring buffer lock is `portENTER_CRITICAL()`, which masks only those interrupts. A higher-priority UART interrupt can preempt the task inside the lock.
- `rscStm32RxDropped` counts the bytes lost because the ring buffer was full. A nonzero value means the buffer is too small or the receive task runs too rarely.

## Upgrading from the 1.0.0 fragment

- RingBuffer 1.x became RngBuffer 2.x: `ringbuffer.h` is now `RngBuffer.h`, the type `RingBuffer` is now `RngBuffer`, and every `ring_buffer_` call is now `rng_buffer_`.
- The lock hooks are now `rng_buffer_lock()` and `rng_buffer_unlock()` in `rngbuffer_stm32.c`. *** IMPORTANT *** Delete the old `ring_buffer_lock()` and `ring_buffer_unlock()`. Overrides left under the old names still compile and link, and the buffer then runs unlocked.
- `rscPlatfromSendBytes()` is now `rscPlatformSendBytes()`.
