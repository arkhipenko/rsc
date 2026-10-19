#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Consistent Overhead Byte Stuffing (Cheshire and Baker, 1999). The encoded bytes contain
// no 0x00, so 0x00 can end a frame. The delimiter is not part of the encoded bytes.

// Largest encoded size of n bytes (one code byte per 254 bytes, at least one)
#define RSC_COBS_MAX_ENCODED(n)   ((n) + (n) / 254 + 1)

// Results of rscCobsDecodeByte()
#define RSC_COBS_MORE         0     // byte taken, no frame yet
#define RSC_COBS_FRAME        1     // a delimiter ended a frame: frameLength bytes in the buffer
#define RSC_COBS_ERROR        (-1)  // a delimiter ended a malformed or oversize frame; it was discarded

// Writes length bytes; returns length, or a negative error
typedef int (*rscCobsWrite_t)( void* ctx, const uint8_t* data, size_t length );

// Encode n bytes into out (RSC_COBS_MAX_ENCODED(n) bytes). Returns the encoded length.
size_t      rscCobsEncode( const uint8_t* in, size_t n, uint8_t* out );

// Encode n bytes block by block through write, without a buffer. No delimiter is written.
// Returns 0, or the first negative result of write (or -1 for a short write).
int         rscCobsEncodeTo( const uint8_t* in, size_t n, rscCobsWrite_t write, void* ctx );

// Decode n encoded bytes (no delimiter) into out. Returns the decoded length, or -1 when
// the input is malformed or longer than cap.
long        rscCobsDecode( const uint8_t* in, size_t n, uint8_t* out, size_t cap );

// Stream decoder: feed received bytes one at a time. After RSC_COBS_FRAME the frame stays in
// buf until the next nonzero byte arrives.
typedef struct {
  uint8_t*  buf;
  size_t    cap;
  size_t    length;         // bytes decoded so far
  size_t    frameLength;    // length of the frame completed by the last delimiter
  uint8_t   code;           // code byte of the current block
  uint8_t   left;           // data bytes left in the current block
  bool      started;        // a code byte of this frame has arrived
  bool      overflow;       // the frame is longer than cap; it is discarded at its delimiter
} rscCobsDecoder_t;

void        rscCobsDecoderInit( rscCobsDecoder_t* d, uint8_t* buf, size_t cap );
int         rscCobsDecodeByte( rscCobsDecoder_t* d, uint8_t b );

#ifdef __cplusplus
}
#endif
