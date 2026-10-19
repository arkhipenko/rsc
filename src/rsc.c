#include <string.h>

#include "rsc.h"

#if RSC_STATS
#define RSC_STAT(l, field)    ((l)->stats.field++)
#else
#define RSC_STAT(l, field)    ((void) 0)
#endif


// Helpers ====================================================================
static uint32_t rscNow(rscLink_t* l) {
  return l->ops.millis(l->ops.ctx);
}

static void rscLock(rscLink_t* l) {
  if ( l->ops.lock ) l->ops.lock(l->ops.ctx);
}

static void rscUnlock(rscLink_t* l) {
  if ( l->ops.unlock ) l->ops.unlock(l->ops.ctx);
}

static bool rscElapsed(uint32_t now, uint32_t start, uint32_t span) {
  return (uint32_t) (now - start) >= span;
}

static uint32_t rscRemaining(uint32_t now, uint32_t start, uint32_t span) {
  uint32_t e = now - start;
  return ( e >= span ) ? 0 : span - e;
}

// Every multi-byte field is little-endian on the wire
static void rscPut16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

static void rscPut32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}

static uint16_t rscGet16(const uint8_t* p) {
  return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rscGet32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Slot i, counted from the oldest queued message
static rscSlot_t* rscSlot(rscLink_t* l, uint8_t i) {
  return &l->slots[(l->head + i) % RSC_TX_SLOTS];
}

// Pending request of tag (under the lock). Tag 0 finds a free entry.
static rscPending_t* rscFindPending(rscLink_t* l, uint8_t tag) {
  for (uint8_t i = 0; i < RSC_MAX_REQUESTS; i++) {
    if ( l->pending[i].tag == tag ) return &l->pending[i];
  }
  return NULL;
}

static void rscNotify(rscLink_t* l, rscResponseCb_t cb, void* user, uint8_t tag, uint8_t event,
                      uint8_t type, int8_t error, const uint8_t* data, uint16_t length) {
  rscResponse_t r;

  if ( cb == NULL ) return;
  r.tag = tag;
  r.event = event;
  r.type = type;
  r.error = error;
  r.data = data;
  r.length = length;
  cb(l, &r, user);
}

// End the pending request tag with an event that has no response frame
static void rscFail(rscLink_t* l, uint8_t tag, uint8_t event) {
  rscResponseCb_t cb = NULL;
  void* user = NULL;
  bool found = false;

  rscLock(l);
  rscPending_t* p = rscFindPending(l, tag);
  if ( p != NULL && tag != 0 ) {
    cb = p->callback;
    user = p->user;
    p->tag = 0;
    found = true;
  }
  rscUnlock(l);
  if ( found ) rscNotify(l, cb, user, tag, event, 0, 0, NULL, 0);
}

static void rscEvent(rscLink_t* l, uint8_t event) {
  if ( l->cfg.onEvent ) l->cfg.onEvent(l, event, l->cfg.user);
}


// Transmit ===================================================================
static uint8_t rscLink0(rscLink_t* l, uint8_t seq, bool reliable) {
  uint8_t b = (uint8_t) ((seq & RSC_L0_SEQ_MASK) | ((l->rxNext & RSC_L0_SEQ_MASK) << RSC_L0_ACK_SHIFT));
  if ( reliable ) b |= RSC_L0_RELIABLE;
  if ( l->rnr ) b |= RSC_L0_RNR;
  return b;
}

// Append the CRC to the n bytes of f (f has room for it), then write the frame COBS encoded,
// with a leading 0x00 after silence and the 0x00 delimiter
static void rscSendFrame(rscLink_t* l, uint8_t* f, size_t n) {
  static const uint8_t zero = 0;
  bool ok = true;

  rscPut16(f + n, rscCrc16(f, n, RSC_CRC16_INIT));
  n += RSC_CRC_LEN;

  if ( !l->txEver || rscElapsed(rscNow(l), l->lastTx, RSC_FLUSH_IDLE_MS) ) {
    ok = l->ops.write(l->ops.ctx, &zero, 1) == 1;
  }
  if ( ok ) ok = rscCobsEncodeTo(f, n, l->ops.write, l->ops.ctx) == 0;
  if ( ok ) ok = l->ops.write(l->ops.ctx, &zero, 1) == 1;
  if ( ok ) RSC_STAT(l, txFrames);
  else RSC_STAT(l, txErrors);
  l->lastTx = rscNow(l);
  l->txEver = true;
}

// Send queued message i with sequence number base + i. It carries the current ACK.
static void rscSendSlot(rscLink_t* l, uint8_t i) {
  rscSlot_t* s = rscSlot(l, i);
  s->frame[0] = rscLink0(l, (uint8_t) (l->base + i), true);
  s->frame[1] = RSC_KIND_DATA;
  rscSendFrame(l, s->frame, RSC_LINK_HEADER_LEN + s->length);
  l->ackDue = false;
}

static void rscSendAck(rscLink_t* l) {
  uint8_t f[RSC_LINK_HEADER_LEN + RSC_CRC_LEN];
  f[0] = rscLink0(l, 0, false);
  f[1] = RSC_KIND_ACK;
  rscSendFrame(l, f, RSC_LINK_HEADER_LEN);
  l->ackDue = false;
}

// SEQ carries the oldest unacknowledged sequence number and ACK the next expected one, so that
// the peer can check that both sides still agree (see rscConsistent)
static void rscSendSync(rscLink_t* l, bool response) {
  uint8_t f[RSC_LINK_HEADER_LEN + RSC_SYNC_RSP_LEN + RSC_CRC_LEN];
  uint8_t* b = f + RSC_LINK_HEADER_LEN;
  size_t n = RSC_SYNC_LEN;

  f[0] = rscLink0(l, l->base, false);
  f[1] = response ? RSC_KIND_SYNC_RSP : RSC_KIND_SYNC;
  b[0] = RSC_PROTOCOL_VERSION;
  b[1] = l->inFlight;
  rscPut16(b + 2, l->cfg.mtu);
  rscPut32(b + 4, l->nonce);
  if ( response ) {
    rscPut32(b + 8, l->syncEcho);
    n = RSC_SYNC_RSP_LEN;
  }
  rscSendFrame(l, f, RSC_LINK_HEADER_LEN + n);
}

// Withdraw a queued message that has not been sent: it goes out as VOID (lock held, or the slot
// is not reachable by other threads)
static void rscVoidSlot(rscSlot_t* s) {
  s->frame[RSC_LINK_HEADER_LEN] = RSC_TYPE_VOID;
  s->frame[RSC_LINK_HEADER_LEN + 1] = 0;
  s->length = RSC_MESSAGE_HEADER_LEN;
}

static bool rscIsRequest(const rscSlot_t* s) {
  uint8_t type = s->frame[RSC_LINK_HEADER_LEN];
  uint8_t tagb = s->frame[RSC_LINK_HEADER_LEN + 1];
  return type != RSC_TYPE_VOID && tagb != 0 && ( tagb & RSC_TAG_RESPONSE ) == 0;
}

// Copy a message into the next free slot (lock held)
static rscError_t rscPushLocked(rscLink_t* l, uint8_t type, uint8_t tagb, int16_t prefix,
                                const uint8_t* data, uint16_t length) {
  if ( l->count >= RSC_TX_SLOTS ) return RSC_ERROR_QUEUE_FULL;

  rscSlot_t* s = rscSlot(l, l->count);
  uint8_t* m = s->frame + RSC_LINK_HEADER_LEN;
  uint16_t n = RSC_MESSAGE_HEADER_LEN;

  m[0] = type;
  m[1] = tagb;
  if ( prefix >= 0 ) m[n++] = (uint8_t) prefix;
  if ( length > 0 ) memcpy(m + n, data, length);
  s->length = (uint16_t) (n + length);
  l->count++;
  return RSC_OK;
}


// Link state =================================================================
static void rscCheckUp(rscLink_t* l) {
  if ( l->up || !l->peerKnown || !l->confirmed ) return;
  l->up = true;
  l->rto = l->rto0;
  l->rtoCount = 0;
  if ( l->inFlight > 0 ) l->resend = true;
  rscEvent(l, RSC_EVENT_UP);
}

static void rscLinkDown(rscLink_t* l) {
  l->up = false;
  l->confirmed = false;
  l->syncDue = true;
  l->timerOn = false;
  l->resend = false;
  l->peerRnr = false;
  l->rto = l->rto0;
  l->rtoCount = 0;
  RSC_STAT(l, linkDowns);
  rscEvent(l, RSC_EVENT_DOWN);
}

// The peer restarted. Frames it never acknowledged are numbered again from 0 and go to the new
// peer. Requests the old peer had received end with RSC_RESPONSE_PEER_RESET.
static void rscPeerReset(rscLink_t* l, uint32_t nonce) {
  l->peerNonce = nonce;
  l->peerKnown = true;
  l->confirmed = false;
  l->up = false;
  l->syncDue = true;
  l->inFlight = 0;
  l->base = 0;
  l->rxNext = 0;
  l->ackDue = false;
  l->peerRnr = false;
  l->timerOn = false;
  l->resend = false;
  l->rto = l->rto0;
  l->rtoCount = 0;
  RSC_STAT(l, peerResets);

  while (true) {
    uint8_t tag = 0;
    rscLock(l);
    for (uint8_t i = 0; i < RSC_MAX_REQUESTS; i++) {
      if ( l->pending[i].tag != 0 && l->pending[i].delivered ) {
        tag = l->pending[i].tag;
        break;
      }
    }
    rscUnlock(l);
    if ( tag == 0 ) break;
    rscFail(l, tag, RSC_RESPONSE_PEER_RESET);
  }
  rscEvent(l, RSC_EVENT_PEER_RESET);
}

// The peer's sequence state (from its SYNC) must fit the local one: the peer expects a frame of
// the local window, and the local side expects a frame of the peer's window. A peer that
// restarted with a repeated nonce fails this check.
static bool rscConsistent(rscLink_t* l, uint8_t peerL0, uint8_t peerInFlight) {
  uint8_t peerSeq = peerL0 & RSC_L0_SEQ_MASK;
  uint8_t peerAck = (peerL0 >> RSC_L0_ACK_SHIFT) & RSC_L0_SEQ_MASK;
  return ((uint8_t) (peerAck - l->base) & RSC_L0_SEQ_MASK) <= l->inFlight &&
         ((uint8_t) (l->rxNext - peerSeq) & RSC_L0_SEQ_MASK) <= peerInFlight;
}

static void rscHandleSync(rscLink_t* l, uint8_t l0, uint8_t kind, const uint8_t* b, size_t n) {
  bool response = ( kind == RSC_KIND_SYNC_RSP );

  if ( n < (size_t) (response ? RSC_SYNC_RSP_LEN : RSC_SYNC_LEN) || b[0] != RSC_PROTOCOL_VERSION ) {
    RSC_STAT(l, rejected);
    return;
  }

  uint8_t peerInFlight = b[1];
  uint16_t mtu = rscGet16(b + 2);
  uint32_t nonce = rscGet32(b + 4);

  if ( peerInFlight > RSC_MAX_WINDOW || mtu == 0 ) {
    RSC_STAT(l, rejected);
    return;
  }
  // a response must answer the current nonce of this side
  if ( response && rscGet32(b + 8) != l->nonce ) {
    RSC_STAT(l, rejected);
    return;
  }

  if ( l->peerKnown && ( nonce != l->peerNonce || !rscConsistent(l, l0, peerInFlight) ) ) {
    rscPeerReset(l, nonce);
  }
  else if ( !l->peerKnown ) {
    l->peerNonce = nonce;
    l->peerKnown = true;
  }
  l->peerMtu = mtu;

  if ( response ) {
    l->confirmed = true;
  }
  else {
    l->syncEcho = nonce;
    l->syncRspDue = true;
  }
  rscCheckUp(l);
}

// The ACK field acknowledges every frame before it. Acknowledged messages leave the queue.
static void rscProcessAck(rscLink_t* l, uint8_t l0) {
  uint8_t ack = (l0 >> RSC_L0_ACK_SHIFT) & RSC_L0_SEQ_MASK;
  uint8_t n = (uint8_t) (ack - l->base) & RSC_L0_SEQ_MASK;

  if ( n > 0 && n <= l->inFlight ) {
    rscLock(l);
    for (uint8_t i = 0; i < n; i++) {
      rscSlot_t* s = &l->slots[l->head];
      if ( rscIsRequest(s) ) {
        rscPending_t* p = rscFindPending(l, s->frame[RSC_LINK_HEADER_LEN + 1]);
        if ( p != NULL ) p->delivered = 1;
      }
      l->head = (uint8_t) ((l->head + 1) % RSC_TX_SLOTS);
      l->count--;
    }
    rscUnlock(l);

    l->inFlight = (uint8_t) (l->inFlight - n);
    l->base = (uint8_t) ((l->base + n) & RSC_L0_SEQ_MASK);
    l->rtoCount = 0;
    l->rto = l->rto0;
    l->timerOn = ( l->inFlight > 0 );
    if ( l->timerOn ) l->rtoStart = rscNow(l);
  }

  // the peer is ready again: send the window at once
  bool rnr = ( l0 & RSC_L0_RNR ) != 0;
  if ( l->peerRnr && !rnr && l->inFlight > 0 ) l->resend = true;
  l->peerRnr = rnr;
}


// Receive ====================================================================
static void rscHandleResponse(rscLink_t* l, uint8_t type, uint8_t tag, const uint8_t* data, uint16_t length) {
  rscResponseCb_t cb = NULL;
  void* user = NULL;
  bool found = false;
  bool interim = ( type == RSC_TYPE_PROGRESS );
  uint8_t event;
  int8_t error = 0;

  if ( type > RSC_TYPE_APP_MAX && type != RSC_TYPE_PROGRESS && type != RSC_TYPE_NACK && type != RSC_TYPE_ACK ) {
    RSC_STAT(l, rejected);
    return;
  }

  rscLock(l);
  rscPending_t* p = ( tag != 0 ) ? rscFindPending(l, tag) : NULL;
  if ( p != NULL ) {
    found = true;
    cb = p->callback;
    user = p->user;
    if ( interim ) p->start = rscNow(l);
    else p->tag = 0;
  }
  rscUnlock(l);

  if ( !found ) {
    RSC_STAT(l, lateResponses);
    return;
  }

  if ( interim ) {
    event = RSC_RESPONSE_PROGRESS;
  }
  else if ( type == RSC_TYPE_NACK ) {
    event = RSC_RESPONSE_NACK;
    error = RSC_ERROR_GENERAL;
    if ( length > 0 ) {
      error = (int8_t) data[0];
      data++;
      length--;
    }
  }
  else if ( type == RSC_TYPE_ACK ) {
    event = RSC_RESPONSE_ACK;
  }
  else {
    event = RSC_RESPONSE_DATA;
  }
  rscNotify(l, cb, user, tag, event, type, error, data, length);
}

// Returns false when the application refused the message
static bool rscDispatch(rscLink_t* l, uint8_t type, uint8_t tagb, const uint8_t* data, uint16_t length) {
  if ( type == RSC_TYPE_VOID ) return true;

  if ( tagb & RSC_TAG_RESPONSE ) {
    rscHandleResponse(l, type, tagb & RSC_TAG_MASK, data, length);
    return true;
  }

  if ( type > RSC_TYPE_APP_MAX ) {
    RSC_STAT(l, rejected);
    return true;
  }
  if ( l->cfg.onMessage == NULL ) return true;

  rscMessage_t m;
  m.type = type;
  m.tag = tagb;
  m.data = data;
  m.length = length;
  return l->cfg.onMessage(l, &m, l->cfg.user);
}

static void rscHandleData(rscLink_t* l, uint8_t l0, const uint8_t* body, size_t n) {
  bool reliable = ( l0 & RSC_L0_RELIABLE ) != 0;

  if ( n < RSC_MESSAGE_HEADER_LEN ) {
    RSC_STAT(l, rejected);
    return;
  }

  if ( reliable ) {
    l->ackDue = true;
    if ( (l0 & RSC_L0_SEQ_MASK) != l->rxNext ) {
      RSC_STAT(l, duplicates);
      return;
    }
    if ( l->rnr ) {
      RSC_STAT(l, busy);
      return;
    }
  }

  if ( !rscDispatch(l, body[0], body[1], body + RSC_MESSAGE_HEADER_LEN, (uint16_t) (n - RSC_MESSAGE_HEADER_LEN)) ) {
    // refused: not acknowledged; the ACK now carries "not ready"
    l->rnr = true;
    RSC_STAT(l, busy);
    return;
  }
  if ( reliable ) l->rxNext = (uint8_t) ((l->rxNext + 1) & RSC_L0_SEQ_MASK);
}

static void rscHandleFrame(rscLink_t* l, const uint8_t* f, size_t n) {
  if ( n < RSC_LINK_HEADER_LEN + RSC_CRC_LEN ) {
    RSC_STAT(l, framingErrors);
    return;
  }
  n -= RSC_CRC_LEN;
  if ( rscCrc16(f, n, RSC_CRC16_INIT) != rscGet16(f + n) ) {
    RSC_STAT(l, crcErrors);
    return;
  }
  RSC_STAT(l, rxFrames);

  uint8_t l0 = f[0];
  uint8_t l1 = f[1];
  const uint8_t* body = f + RSC_LINK_HEADER_LEN;
  size_t bodyLen = n - RSC_LINK_HEADER_LEN;

  // reserved bits, and segments (2.1.0)
  if ( l1 & (RSC_L1_RESERVED | RSC_L1_MORE) ) {
    RSC_STAT(l, rejected);
    return;
  }

  switch ( l1 & RSC_L1_KIND_MASK ) {
    case RSC_KIND_SYNC:
    case RSC_KIND_SYNC_RSP:
      rscHandleSync(l, l0, l1 & RSC_L1_KIND_MASK, body, bodyLen);
      break;

    case RSC_KIND_ACK:
      if ( l->up ) rscProcessAck(l, l0);
      break;

    case RSC_KIND_DATA:
      if ( !l->up ) break;
      rscProcessAck(l, l0);
      rscHandleData(l, l0, body, bodyLen);
      break;

    default:
      RSC_STAT(l, rejected);
      break;
  }
}

void rscFeed( rscLink_t* link, const uint8_t* data, size_t length ) {
  if ( link == NULL || data == NULL ) return;

  for (size_t i = 0; i < length; i++) {
    int r = rscCobsDecodeByte(&link->dec, data[i]);
    if ( r == RSC_COBS_FRAME ) rscHandleFrame(link, link->rx, link->dec.frameLength);
    else if ( r == RSC_COBS_ERROR ) RSC_STAT(link, framingErrors);
  }
}


// Service ====================================================================
// Requests past their timeout end with RSC_RESPONSE_TIMEOUT. One that has not left yet is
// withdrawn: it goes out as VOID.
static void rscCheckRequests(rscLink_t* l, uint32_t now) {
  while (true) {
    rscResponseCb_t cb = NULL;
    void* user = NULL;
    uint8_t tag = 0;

    rscLock(l);
    for (uint8_t i = 0; i < RSC_MAX_REQUESTS; i++) {
      rscPending_t* p = &l->pending[i];
      if ( p->tag != 0 && p->timeout != 0 && rscElapsed(now, p->start, p->timeout) ) {
        tag = p->tag;
        cb = p->callback;
        user = p->user;
        p->tag = 0;
        for (uint8_t k = l->inFlight; k < l->count; k++) {
          rscSlot_t* s = rscSlot(l, k);
          if ( rscIsRequest(s) && s->frame[RSC_LINK_HEADER_LEN + 1] == tag ) {
            rscVoidSlot(s);
            break;
          }
        }
        break;
      }
    }
    rscUnlock(l);

    if ( tag == 0 ) break;
    rscNotify(l, cb, user, tag, RSC_RESPONSE_TIMEOUT, 0, 0, NULL, 0);
  }
}

static bool rscCanSendNew(rscLink_t* l) {
  rscLock(l);
  uint8_t count = l->count;
  rscUnlock(l);
  return l->up && l->inFlight < count && l->inFlight < l->cfg.window && ( !l->peerRnr || l->inFlight == 0 );
}

uint32_t rscService( rscLink_t* link ) {
  rscLink_t* l = link;
  if ( l == NULL ) return RSC_IDLE_MS;

  uint32_t now = rscNow(l);

  rscLock(l);
  if ( l->resumeReq ) {
    l->resumeReq = false;
    if ( l->rnr ) {
      l->rnr = false;
      l->ackDue = true;
    }
  }
  rscUnlock(l);

  rscCheckRequests(l, now);

  // session
  if ( !l->confirmed && ( l->syncDue || rscElapsed(now, l->lastSync, RSC_SYNC_INTERVAL_MS) ) ) {
    rscSendSync(l, false);
    l->lastSync = now;
    l->syncDue = false;
  }
  if ( l->syncRspDue ) {
    rscSendSync(l, true);
    l->syncRspDue = false;
  }

  // retransmission timer
  if ( l->up && l->timerOn && rscElapsed(now, l->rtoStart, l->rto) ) {
    uint32_t longest = l->rto0 * RSC_RTO_MAX_FACTOR;
    RSC_STAT(l, timeouts);
    if ( l->peerRnr ) {
      // the peer is not ready: probe at the longest timeout, without counting a retry
      l->rto = longest;
      l->resend = true;
    }
    else if ( ++l->rtoCount > l->cfg.retries ) {
      rscLinkDown(l);
    }
    else {
      l->rto = ( l->rto * 2 > longest ) ? longest : l->rto * 2;
      l->resend = true;
    }
  }

  if ( l->up && l->resend ) {
    l->resend = false;
    for (uint8_t i = 0; i < l->inFlight; i++) {
      rscSendSlot(l, i);
      RSC_STAT(l, retransmits);
    }
    if ( l->inFlight > 0 ) {
      l->timerOn = true;
      l->rtoStart = rscNow(l);
    }
  }

  // new frames, up to the window
  while ( rscCanSendNew(l) ) {
    rscSlot_t* s = rscSlot(l, l->inFlight);
    if ( s->length - RSC_MESSAGE_HEADER_LEN > rscMtu(l) ) {
      // longer than the peer accepts: sent as VOID, and its request fails
      bool request = rscIsRequest(s);
      uint8_t tag = s->frame[RSC_LINK_HEADER_LEN + 1];
      rscLock(l);
      rscVoidSlot(s);
      rscUnlock(l);
      RSC_STAT(l, dropped);
      if ( request ) rscFail(l, tag, RSC_RESPONSE_DROPPED);
    }
    rscSendSlot(l, l->inFlight);
    l->inFlight++;
    if ( !l->timerOn ) {
      l->timerOn = true;
      l->rtoStart = rscNow(l);
    }
  }

  if ( l->up && l->ackDue ) rscSendAck(l);

  // time until the next deadline
  now = rscNow(l);
  uint32_t wait = RSC_IDLE_MS;
  uint32_t r;
  if ( !l->confirmed ) {
    r = rscRemaining(now, l->lastSync, RSC_SYNC_INTERVAL_MS);
    if ( r < wait ) wait = r;
  }
  if ( l->up && l->timerOn ) {
    r = rscRemaining(now, l->rtoStart, l->rto);
    if ( r < wait ) wait = r;
  }
  rscLock(l);
  for (uint8_t i = 0; i < RSC_MAX_REQUESTS; i++) {
    rscPending_t* p = &l->pending[i];
    if ( p->tag != 0 && p->timeout != 0 ) {
      r = rscRemaining(now, p->start, p->timeout);
      if ( r < wait ) wait = r;
    }
  }
  if ( l->resumeReq ) wait = 0;
  rscUnlock(l);
  if ( l->ackDue || l->syncRspDue || l->resend || rscCanSendNew(l) ) wait = 0;
  return wait;
}


// API ========================================================================
void rscConfigDefaults( rscConfig_t* cfg ) {
  if ( cfg == NULL ) return;
  memset(cfg, 0, sizeof(rscConfig_t));
  cfg->mtu = RSC_MTU;
  cfg->window = RSC_WINDOW;
  cfg->retries = RSC_RETRIES;
}

// Fallback nonce when ops->random is missing: the time and the link address, mixed
static uint32_t rscMix(uint32_t x) {
  x ^= x >> 16;
  x *= (uint32_t) 0x85EBCA6B;
  x ^= x >> 13;
  x *= (uint32_t) 0xC2B2AE35;
  x ^= x >> 16;
  return x;
}

rscError_t rscInit( rscLink_t* link, const rscConfig_t* cfg, const rscOps_t* ops ) {
  if ( link == NULL || cfg == NULL || ops == NULL || ops->write == NULL || ops->millis == NULL ) return RSC_ERROR_NULL;
  if ( (ops->lock == NULL) != (ops->unlock == NULL) ) return RSC_ERROR_BAD_ARG;
  if ( cfg->mtu > RSC_MTU || cfg->window > RSC_MAX_WINDOW ) return RSC_ERROR_BAD_ARG;

  memset(link, 0, sizeof(rscLink_t));
  link->ops = *ops;
  link->cfg = *cfg;
  if ( link->cfg.mtu == 0 ) link->cfg.mtu = RSC_MTU;
  if ( link->cfg.window == 0 ) link->cfg.window = RSC_WINDOW;
  if ( link->cfg.retries == 0 ) link->cfg.retries = RSC_RETRIES;

  // first retransmission timeout: 3 maximum frames on the line plus a margin
  if ( link->cfg.rtoMs != 0 ) {
    link->rto0 = link->cfg.rtoMs;
  }
  else if ( link->cfg.baud != 0 ) {
    uint32_t frameBytes = RSC_LINK_HEADER_LEN + RSC_MESSAGE_HEADER_LEN + link->cfg.mtu + RSC_CRC_LEN;
    uint32_t wireBits = (frameBytes + frameBytes / 254 + 3) * 10;      // COBS, two delimiters, start and stop bits
    uint32_t frameMs = (uint32_t) ((wireBits * (uint32_t) 1000 + link->cfg.baud - 1) / link->cfg.baud);
    link->rto0 = RSC_RTO_MIN_MS + 3 * frameMs;
  }
  else {
    link->rto0 = RSC_RTO_DEFAULT_MS;
  }
  link->rto = link->rto0;

  link->peerMtu = link->cfg.mtu;
  link->nonce = ops->random ? ops->random(ops->ctx)
                            : rscMix(ops->millis(ops->ctx) ^ (uint32_t) (uintptr_t) (void*) link);
  if ( link->nonce == 0 ) link->nonce = 1;
  link->nextTag = 1;
  link->syncDue = true;
  rscCobsDecoderInit(&link->dec, link->rx, sizeof(link->rx));
  return RSC_OK;
}

static rscError_t rscQueue(rscLink_t* l, uint8_t type, uint8_t tagb, int16_t prefix, const uint8_t* data, uint16_t length) {
  if ( l == NULL || ( length > 0 && data == NULL ) ) return RSC_ERROR_NULL;
  if ( (uint32_t) length + ( prefix >= 0 ? 1 : 0 ) > rscMtu(l) ) return RSC_ERROR_OVERFLOW;

  rscLock(l);
  rscError_t rc = rscPushLocked(l, type, tagb, prefix, data, length);
  rscUnlock(l);
  if ( rc == RSC_OK && l->ops.wake ) l->ops.wake(l->ops.ctx);
  return rc;
}

rscError_t rscSend( rscLink_t* link, uint8_t type, const uint8_t* data, uint16_t length ) {
  if ( type > RSC_TYPE_APP_MAX ) return RSC_ERROR_BAD_ARG;
  return rscQueue(link, type, 0, -1, data, length);
}

rscError_t rscRequest( rscLink_t* link, uint8_t type, const uint8_t* data, uint16_t length,
                       uint32_t timeoutMs, rscResponseCb_t callback, void* user, uint8_t* tag ) {
  rscLink_t* l = link;
  if ( l == NULL || ( length > 0 && data == NULL ) ) return RSC_ERROR_NULL;
  if ( type > RSC_TYPE_APP_MAX ) return RSC_ERROR_BAD_ARG;
  if ( length > rscMtu(l) ) return RSC_ERROR_OVERFLOW;

  rscLock(l);
  rscPending_t* p = rscFindPending(l, 0);
  if ( p == NULL ) {
    rscUnlock(l);
    return RSC_ERROR_NO_TAG;
  }
  if ( l->count >= RSC_TX_SLOTS ) {
    rscUnlock(l);
    return RSC_ERROR_QUEUE_FULL;
  }

  // the next tag not in use (a free entry exists, so at most 126 tags are taken)
  uint8_t t = l->nextTag;
  while ( rscFindPending(l, t) != NULL ) t = (uint8_t) (t % RSC_TAG_MASK + 1);
  l->nextTag = (uint8_t) (t % RSC_TAG_MASK + 1);

  p->callback = callback;
  p->user = user;
  p->start = rscNow(l);
  p->timeout = timeoutMs;
  p->tag = t;
  p->delivered = 0;
  rscPushLocked(l, type, t, -1, data, length);
  rscUnlock(l);

  if ( l->ops.wake ) l->ops.wake(l->ops.ctx);
  if ( tag ) *tag = t;
  return RSC_OK;
}

static rscError_t rscAnswer(rscLink_t* l, uint8_t tag, uint8_t type, int16_t prefix, const uint8_t* data, uint16_t length) {
  if ( tag == 0 || tag > RSC_TAG_MASK ) return RSC_ERROR_BAD_ARG;
  return rscQueue(l, type, (uint8_t) (tag | RSC_TAG_RESPONSE), prefix, data, length);
}

rscError_t rscRespond( rscLink_t* link, uint8_t tag, uint8_t type, const uint8_t* data, uint16_t length ) {
  if ( type > RSC_TYPE_APP_MAX ) return RSC_ERROR_BAD_ARG;
  return rscAnswer(link, tag, type, -1, data, length);
}

rscError_t rscAck( rscLink_t* link, uint8_t tag, const uint8_t* data, uint16_t length ) {
  return rscAnswer(link, tag, RSC_TYPE_ACK, -1, data, length);
}

rscError_t rscNack( rscLink_t* link, uint8_t tag, int8_t error, const uint8_t* data, uint16_t length ) {
  return rscAnswer(link, tag, RSC_TYPE_NACK, (int16_t) (uint8_t) error, data, length);
}

rscError_t rscProgress( rscLink_t* link, uint8_t tag, const uint8_t* data, uint16_t length ) {
  return rscAnswer(link, tag, RSC_TYPE_PROGRESS, -1, data, length);
}

void rscResume( rscLink_t* link ) {
  if ( link == NULL ) return;
  rscLock(link);
  link->resumeReq = true;
  rscUnlock(link);
  if ( link->ops.wake ) link->ops.wake(link->ops.ctx);
}

bool rscIsUp( const rscLink_t* link ) {
  return link != NULL && link->up;
}

uint16_t rscMtu( const rscLink_t* link ) {
  if ( link == NULL ) return 0;
  if ( link->peerKnown && link->peerMtu < link->cfg.mtu ) return link->peerMtu;
  return link->cfg.mtu;
}

uint8_t rscQueued( const rscLink_t* link ) {
  return link ? link->count : 0;
}

#if RSC_STATS
const rscStats_t* rscGetStats( const rscLink_t* link ) {
  return link ? &link->stats : NULL;
}
#endif
