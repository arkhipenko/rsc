#include "rsc_cobs.h"

// Length of the next block: nonzero bytes from i, at most 254
static size_t rscCobsRun(const uint8_t* in, size_t n, size_t i) {
  size_t j = i;
  while ( j < n && in[j] != 0 && j - i < 254 ) j++;
  return j - i;
}

size_t rscCobsEncode( const uint8_t* in, size_t n, uint8_t* out ) {
  size_t i = 0, o = 0;

  while (true) {
    size_t run = rscCobsRun(in, n, i);
    out[o++] = (uint8_t) (run + 1);
    for (size_t k = 0; k < run; k++) out[o++] = in[i + k];
    i += run;
    if ( i >= n ) break;
    if ( run < 254 ) i++;           // the zero that ended the block is implied by its code
  }
  return o;
}

int rscCobsEncodeTo( const uint8_t* in, size_t n, rscCobsWrite_t write, void* ctx ) {
  size_t i = 0;

  while (true) {
    size_t run = rscCobsRun(in, n, i);
    uint8_t code = (uint8_t) (run + 1);
    int rc = write(ctx, &code, 1);
    if ( rc < 0 ) return rc;
    if ( rc != 1 ) return -1;
    if ( run > 0 ) {
      rc = write(ctx, in + i, run);
      if ( rc < 0 ) return rc;
      if ( (size_t) rc != run ) return -1;
    }
    i += run;
    if ( i >= n ) break;
    if ( run < 254 ) i++;
  }
  return 0;
}

void rscCobsDecoderInit( rscCobsDecoder_t* d, uint8_t* buf, size_t cap ) {
  d->buf = buf;
  d->cap = cap;
  d->length = 0;
  d->frameLength = 0;
  d->code = 0;
  d->left = 0;
  d->started = false;
  d->overflow = false;
}

static void rscCobsPut(rscCobsDecoder_t* d, uint8_t b) {
  if ( d->length >= d->cap ) {
    d->overflow = true;
    return;
  }
  d->buf[d->length++] = b;
}

int rscCobsDecodeByte( rscCobsDecoder_t* d, uint8_t b ) {
  if ( b == 0 ) {
    int r = RSC_COBS_MORE;                              // an empty frame is ignored
    if ( d->overflow || d->left != 0 ) r = RSC_COBS_ERROR;  // too long, or a block cut short
    else if ( d->started ) r = RSC_COBS_FRAME;
    d->frameLength = ( r == RSC_COBS_FRAME ) ? d->length : 0;
    d->length = 0;
    d->code = 0;
    d->left = 0;
    d->started = false;
    d->overflow = false;
    return r;
  }

  if ( d->overflow ) return RSC_COBS_MORE;

  if ( d->left == 0 ) {
    // a code byte: the previous block (if any) ended with an implied zero, unless it was full
    if ( d->started && d->code != 0xFF ) rscCobsPut(d, 0);
    d->code = b;
    d->left = (uint8_t) (b - 1);
    d->started = true;
    return RSC_COBS_MORE;
  }

  rscCobsPut(d, b);
  d->left--;
  return RSC_COBS_MORE;
}

long rscCobsDecode( const uint8_t* in, size_t n, uint8_t* out, size_t cap ) {
  rscCobsDecoder_t d;

  rscCobsDecoderInit(&d, out, cap);
  for (size_t i = 0; i < n; i++) {
    if ( in[i] == 0 ) return -1;
    rscCobsDecodeByte(&d, in[i]);
  }
  if ( rscCobsDecodeByte(&d, 0) != RSC_COBS_FRAME ) return -1;
  return (long) d.frameLength;
}
