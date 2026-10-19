# STM32 platform layer (STM32CubeIDE fragment)

This folder runs one ReliableSerialComms link over USART1 on STM32 HAL with FreeRTOS. It is a fragment of an STM32CubeIDE project. It does not build on its own, and CI does not compile it.

## Files

- `rsc_stm32.c`: the platform operations, the UART receive interrupt callback, `rscStm32Init()` and the task function `startRscTask()`.
- `rsc_stm32.h`: the link `rscStm32Link`, the dropped-byte counter, and the two functions.
- `rngbuffer_stm32.c`: the lock hooks of RngBuffer 2.x (a FreeRTOS critical section).

## What the project supplies

1. The library sources: `src/rsc.c`, `src/rsc_cobs.c`, `src/rsc_crc.c` and the headers of this repository.
2. RngBuffer 2.0.1 (https://github.com/arkhipenko/RngBuffer, tag v2.0.1): `RngBuffer.c` and `RngBuffer.h`.
3. USART1 with its global interrupt enabled, and the handle `UART_HandleTypeDef huart1` (CubeMX generates it).
4. `RngBuffer rxRingBuffer`, initialized with `rng_buffer_init()` over a static byte array before the scheduler starts.
5. One call of `rscStm32Init(onMessage, onEvent)` after the ring buffer is initialized and before the scheduler starts. It creates the link mutex and arms the receive interrupt.
6. One task with the entry function `startRscTask` (stack of at least 512 words with the default RSC_MTU of 256).

Other tasks queue messages on `rscStm32Link` with `rscSend()`, `rscRequest()`, `rscAck()` and the other queueing functions. Callbacks run in the link task.

## Requirements

- `configTICK_RATE_HZ` must divide 1000 (for example 1000 or 100). The millisecond clock uses `portTICK_PERIOD_MS`, which is an integer.
- *** IMPORTANT *** The USART1 interrupt priority must be numerically equal to or greater than `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`. The interrupt calls `vTaskNotifyGiveFromISR()`, and the ring buffer lock is `portENTER_CRITICAL()`, which masks only those interrupts.
- `rscStm32RxDropped` counts the bytes lost because the ring buffer was full. A nonzero value means the buffer is too small. The link recovers the lost frames by retransmission, at a cost in throughput.
- `opRandom()` mixes the SysTick counter and the tick count. On a part with an RNG peripheral, return its value instead, so that the peer recognizes every restart of this board.

## Upgrading from the 1.x fragment

- The two tasks (`startDataRx`, `startDataTx`) and the two queues (`qRxHandle`, `qTxHandle`) are gone. One task runs the link, and the library holds its own queue.
- `rsc_framework_stm32.c` is now `rsc_stm32.c`. `rscQueueEntry_t` and `TASK_INITIAL_DELAY` are gone.
