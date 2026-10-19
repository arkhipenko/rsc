#pragma once

// ReliableSerialComms: the header named after the library (Arduino). Other builds may include
// rsc.h directly.

#include "rsc.h"

#if defined(ARDUINO) && defined(__cplusplus)
#include "rsc_arduino.h"
#endif
