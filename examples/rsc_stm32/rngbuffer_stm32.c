#include <stdlib.h>

#include "main.h"
#include "cmsis_os.h"

#include <RngBuffer.h>


/**
 * @brief Implementation of the locking for ring buffer (RngBuffer 2.x hook)
 *        case CRITICAL Section is used. In an interrupt nothing is done: the task
 *        holds the lock with interrupts masked, so the interrupt cannot preempt it.
 *
 * @return true - always
 */
bool rng_buffer_lock(unsigned int timeout) {
  (void) timeout;
  if ( !__get_IPSR() ) portENTER_CRITICAL();
  return true;
}

/**
 * @brief Implementation of the unlocking for ring buffer (RngBuffer 2.x hook)
 *        case CRITICAL Section is used
 *
 */
void rng_buffer_unlock(void) {
	if ( !__get_IPSR() ) portEXIT_CRITICAL();
}
