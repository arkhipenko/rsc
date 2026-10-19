// Host tests for ReliableSerialComms 2.x: two links joined by a simulated wire with faults.
// Run all tests, or one: ./test_rsc <test name>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <random>
#include <string>
#include <vector>

#include "rsc.h"

static int failures = 0;

#define CHECK(cond) do { \
    if ( !(cond) ) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
  } while (0)

#define CHECK_EQ(a, b) do { \
    long long va_ = (long long) (a), vb_ = (long long) (b); \
    if ( va_ != vb_ ) { printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); failures++; } \
  } while (0)

typedef std::vector<uint8_t> Bytes;

static Bytes hexBytes(const char* hex) {
  Bytes b;
  unsigned v;
  int n;
  while ( sscanf(hex, " %2x%n", &v, &n) == 1 ) {
    b.push_back((uint8_t) v);
    hex += n;
  }
  return b;
}


// Simulation =================================================================
static uint32_t simNow = 1000;

struct Msg {
  uint8_t type;
  uint8_t tag;
  Bytes   data;
};

struct Rsp {
  uint8_t tag;
  uint8_t event;
  uint8_t type;
  int8_t  error;
  Bytes   data;
};

struct Peer {
  const char*       name;
  rscLink_t         link;
  rscOps_t          ops;
  rscConfig_t       cfg;
  Bytes             out;                // written, not yet delivered
  Bytes             capture;            // every byte written while capturing
  bool              capturing;
  uint32_t          nonce;              // 0: random
  std::mt19937      rng;
  std::vector<Msg>  msgs;
  std::vector<Rsp>  rsps;
  std::vector<uint8_t> events;
  std::vector<std::pair<uint32_t, uint16_t> > deferred;  // (time, tag | 0x100 for a final ACK)
  std::function<bool(Peer&, const rscMessage_t*)> handler;
  int               lockDepth;
  int               maxLockDepth;
  bool              writeFails;

  Peer(const char* n, uint32_t seed) : name(n), capturing(false), nonce(0), rng(seed),
                                       lockDepth(0), maxLockDepth(0), writeFails(false) {
    memset(&link, 0, sizeof(link));
  }
};

static int opWrite(void* ctx, const uint8_t* data, size_t length) {
  Peer* p = (Peer*) ctx;
  if ( p->writeFails ) return -1;
  p->out.insert(p->out.end(), data, data + length);
  if ( p->capturing ) p->capture.insert(p->capture.end(), data, data + length);
  return (int) length;
}

static uint32_t opMillis(void* ctx) {
  (void) ctx;
  return simNow;
}

static uint32_t opRandom(void* ctx) {
  Peer* p = (Peer*) ctx;
  return p->nonce ? p->nonce : (uint32_t) p->rng();
}

static void opLock(void* ctx) {
  Peer* p = (Peer*) ctx;
  p->lockDepth++;
  if ( p->lockDepth > p->maxLockDepth ) p->maxLockDepth = p->lockDepth;
}

static void opUnlock(void* ctx) {
  ((Peer*) ctx)->lockDepth--;
}

// The default responder: answers each request type in its own way
static bool defaultHandler(Peer& p, const rscMessage_t* m) {
  if ( m->tag == 0 ) return true;
  switch ( m->type ) {
    case 0x01: rscAck(&p.link, m->tag, m->data, m->length); break;
    case 0x02: rscNack(&p.link, m->tag, RSC_ERROR_CMD_BAD_ARG, (const uint8_t*) "no", 2); break;
    case 0x03: rscRespond(&p.link, m->tag, 0x33, m->data, m->length); break;
    case 0x04:      // accepted now, done in 300 ms
      rscProgress(&p.link, m->tag, NULL, 0);
      p.deferred.push_back(std::make_pair(simNow + 300, (uint16_t) (m->tag | 0x100)));
      break;
    case 0x06:      // accepted now, still busy at 200 ms, done at 400 ms
      rscProgress(&p.link, m->tag, NULL, 0);
      p.deferred.push_back(std::make_pair(simNow + 200, (uint16_t) m->tag));
      p.deferred.push_back(std::make_pair(simNow + 400, (uint16_t) (m->tag | 0x100)));
      break;
    default: break;   // 0x05 and others: no response
  }
  return true;
}

// Callbacks never run with the lock held, so they may queue messages
static bool onMessage(rscLink_t* link, const rscMessage_t* m, void* user) {
  (void) link;
  Peer* p = (Peer*) user;
  CHECK_EQ(p->lockDepth, 0);
  if ( p->handler && !p->handler(*p, m) ) return false;
  Msg r;
  r.type = m->type;
  r.tag = m->tag;
  r.data.assign(m->data, m->data + m->length);
  p->msgs.push_back(r);
  return true;
}

static void onResponse(rscLink_t* link, const rscResponse_t* rsp, void* user) {
  (void) link;
  Peer* p = (Peer*) user;
  CHECK_EQ(p->lockDepth, 0);
  Rsp r;
  r.tag = rsp->tag;
  r.event = rsp->event;
  r.type = rsp->type;
  r.error = rsp->error;
  if ( rsp->length ) r.data.assign(rsp->data, rsp->data + rsp->length);
  p->rsps.push_back(r);
}

static void onEvent(rscLink_t* link, uint8_t event, void* user) {
  (void) link;
  CHECK_EQ(((Peer*) user)->lockDepth, 0);
  ((Peer*) user)->events.push_back(event);
}

static rscError_t initPeer(Peer& p, uint16_t mtu = 0, uint8_t window = 0, bool withLock = true) {
  memset(&p.ops, 0, sizeof(p.ops));
  p.ops.write = opWrite;
  p.ops.millis = opMillis;
  p.ops.random = opRandom;
  if ( withLock ) {
    p.ops.lock = opLock;
    p.ops.unlock = opUnlock;
  }
  p.ops.ctx = &p;
  rscConfigDefaults(&p.cfg);
  p.cfg.rtoMs = 40;
  if ( mtu ) p.cfg.mtu = mtu;
  if ( window ) p.cfg.window = window;
  p.cfg.onMessage = onMessage;
  p.cfg.onEvent = onEvent;
  p.cfg.user = &p;
  if ( !p.handler ) p.handler = defaultHandler;
  p.out.clear();
  return rscInit(&p.link, &p.cfg, &p.ops);
}

struct Wire {
  double        drop;       // probability per byte
  double        flip;       // probability per byte of one flipped bit
  double        insert;     // probability per byte of an extra random byte
  bool          cut;        // nothing arrives
  bool          oneByte;    // deliver one byte per step
  std::mt19937  rng;
  Wire() : drop(0), flip(0), insert(0), cut(false), oneByte(false), rng(7) {}
};

static void deliver(Peer& from, Peer& to, Wire& w) {
  if ( from.out.empty() ) return;
  if ( w.cut ) {
    from.out.clear();
    return;
  }
  size_t n = w.oneByte ? 1 : from.out.size();
  Bytes chunk(from.out.begin(), from.out.begin() + n);
  from.out.erase(from.out.begin(), from.out.begin() + n);

  std::uniform_real_distribution<double> u(0.0, 1.0);
  Bytes got;
  for (size_t i = 0; i < chunk.size(); i++) {
    if ( w.insert > 0 && u(w.rng) < w.insert ) got.push_back((uint8_t) w.rng());
    if ( w.drop > 0 && u(w.rng) < w.drop ) continue;
    uint8_t b = chunk[i];
    if ( w.flip > 0 && u(w.rng) < w.flip ) b ^= (uint8_t) (1u << (w.rng() % 8));
    got.push_back(b);
  }
  if ( !got.empty() ) rscFeed(&to.link, got.data(), got.size());
}

static void runDeferred(Peer& p) {
  for (size_t i = 0; i < p.deferred.size(); ) {
    if ( (int32_t) (simNow - p.deferred[i].first) >= 0 ) {
      uint8_t tag = (uint8_t) p.deferred[i].second;
      if ( p.deferred[i].second & 0x100 ) rscAck(&p.link, tag, (const uint8_t*) "done", 4);
      else rscProgress(&p.link, tag, (const uint8_t*) "\x32", 1);
      p.deferred.erase(p.deferred.begin() + i);
    }
    else i++;
  }
}

// Advance the simulation by ms milliseconds
static void pump(Peer& a, Peer& b, Wire& w, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t++) {
    runDeferred(a);
    runDeferred(b);
    rscService(&a.link);
    rscService(&b.link);
    deliver(a, b, w);
    deliver(b, a, w);
    rscService(&a.link);
    rscService(&b.link);
    simNow++;
  }
}

// Pump until cond() holds, at most ms milliseconds
static bool pumpUntil(Peer& a, Peer& b, Wire& w, uint32_t ms, std::function<bool()> cond) {
  for (uint32_t t = 0; t < ms; t++) {
    if ( cond() ) return true;
    pump(a, b, w, 1);
  }
  return cond();
}

static bool connect(Peer& a, Peer& b, Wire& w) {
  return pumpUntil(a, b, w, 3000, [&]() { return rscIsUp(&a.link) && rscIsUp(&b.link); });
}

static int countEvent(const Peer& p, uint8_t e) {
  int n = 0;
  for (size_t i = 0; i < p.events.size(); i++) if ( p.events[i] == e ) n++;
  return n;
}

static const Rsp* findRsp(const Peer& p, uint8_t tag, bool final = true) {
  for (size_t i = 0; i < p.rsps.size(); i++) {
    if ( p.rsps[i].tag == tag && ( !final || p.rsps[i].event != RSC_RESPONSE_PROGRESS ) ) return &p.rsps[i];
  }
  return NULL;
}

// Queue an event, pumping while the queue is full
static void sendRetry(Peer& a, Peer& b, Wire& w, uint8_t type, const Bytes& data) {
  for (int i = 0; i < 20000; i++) {
    if ( rscSend(&a.link, type, data.data(), (uint16_t) data.size()) == RSC_OK ) return;
    pump(a, b, w, 1);
  }
  CHECK(false);
}

static uint8_t requestRetry(Peer& a, Peer& b, Wire& w, uint8_t type, const Bytes& data, uint32_t timeout) {
  uint8_t tag = 0;
  for (int i = 0; i < 20000; i++) {
    if ( rscRequest(&a.link, type, data.data(), (uint16_t) data.size(), timeout, onResponse, &a, &tag) == RSC_OK ) return tag;
    pump(a, b, w, 1);
  }
  CHECK(false);
  return 0;
}


// Tests ======================================================================
static void test_crc_check_value() {
  const char* s = "123456789";
  CHECK_EQ(rscCrc16((const uint8_t*) s, 9, RSC_CRC16_INIT), 0x29B1);
  // continuing a CRC over two parts gives the CRC of the whole
  CHECK_EQ(rscCrc16((const uint8_t*) s + 4, 5, rscCrc16((const uint8_t*) s, 4, RSC_CRC16_INIT)), 0x29B1);
}

static void cobsCase(const Bytes& in, const Bytes& expected) {
  std::vector<uint8_t> out(RSC_COBS_MAX_ENCODED(in.size()) + 1);
  size_t n = rscCobsEncode(in.data(), in.size(), out.data());
  CHECK(Bytes(out.begin(), out.begin() + n) == expected);

  Bytes streamed;
  CHECK_EQ(rscCobsEncodeTo(in.data(), in.size(), [](void* ctx, const uint8_t* d, size_t len) -> int {
    Bytes* s = (Bytes*) ctx;
    s->insert(s->end(), d, d + len);
    return (int) len;
  }, &streamed), 0);
  CHECK(streamed == expected);

  Bytes back(in.size() + 1);
  long m = rscCobsDecode(expected.data(), expected.size(), back.data(), back.size());
  CHECK_EQ(m, (long) in.size());
  CHECK(Bytes(back.begin(), back.begin() + (m > 0 ? m : 0)) == in);
}

static void test_cobs_vectors() {
  // the examples of the COBS paper and its common summaries
  cobsCase(hexBytes("00"), hexBytes("01 01"));
  cobsCase(hexBytes("00 00"), hexBytes("01 01 01"));
  cobsCase(hexBytes("00 11 00"), hexBytes("01 02 11 01"));
  cobsCase(hexBytes("11 22 00 33"), hexBytes("03 11 22 02 33"));
  cobsCase(hexBytes("11 22 33 44"), hexBytes("05 11 22 33 44"));
  cobsCase(hexBytes("11 00 00 00"), hexBytes("02 11 01 01 01"));

  Bytes in, exp;
  for (int i = 1; i <= 254; i++) in.push_back((uint8_t) i);              // 01..FE
  exp.push_back(0xFF); exp.insert(exp.end(), in.begin(), in.end());
  cobsCase(in, exp);

  in.clear(); exp.clear();
  for (int i = 0; i <= 254; i++) in.push_back((uint8_t) i);              // 00..FE
  exp.push_back(0x01); exp.push_back(0xFF);
  for (int i = 1; i <= 254; i++) exp.push_back((uint8_t) i);
  cobsCase(in, exp);

  in.clear(); exp.clear();
  for (int i = 1; i <= 255; i++) in.push_back((uint8_t) i);              // 01..FF
  exp.push_back(0xFF);
  for (int i = 1; i <= 254; i++) exp.push_back((uint8_t) i);
  exp.push_back(0x02); exp.push_back(0xFF);
  cobsCase(in, exp);

  in.clear(); exp.clear();
  for (int i = 2; i <= 255; i++) in.push_back((uint8_t) i);              // 02..FF 00
  in.push_back(0x00);
  exp.push_back(0xFF);
  for (int i = 2; i <= 255; i++) exp.push_back((uint8_t) i);
  exp.push_back(0x01); exp.push_back(0x01);
  cobsCase(in, exp);

  in.clear(); exp.clear();
  for (int i = 3; i <= 255; i++) in.push_back((uint8_t) i);              // 03..FF 00 01
  in.push_back(0x00); in.push_back(0x01);
  exp.push_back(0xFE);
  for (int i = 3; i <= 255; i++) exp.push_back((uint8_t) i);
  exp.push_back(0x02); exp.push_back(0x01);
  cobsCase(in, exp);

  // malformed input
  uint8_t buf[16];
  Bytes cut = hexBytes("03 11");                 // a block cut short
  CHECK_EQ(rscCobsDecode(cut.data(), cut.size(), buf, sizeof(buf)), -1);
  Bytes zero = hexBytes("02 00 11");             // a 0x00 inside
  CHECK_EQ(rscCobsDecode(zero.data(), zero.size(), buf, sizeof(buf)), -1);
  Bytes big = hexBytes("05 11 22 33 44");        // longer than cap
  CHECK_EQ(rscCobsDecode(big.data(), big.size(), buf, 3), -1);
  CHECK_EQ(rscCobsDecode(big.data(), big.size(), buf, 4), 4);
}

// Bring two links up with fixed nonces, then check frames written byte for byte
static void test_golden_frames() {
#if RSC_MTU == 256
  Peer a("A", 1), b("B", 2);
  Wire w;
  a.nonce = 0x12345678;
  b.nonce = 0xCAFEBABE;
  a.capturing = true;
  CHECK_EQ(initPeer(a), RSC_OK);
  CHECK_EQ(initPeer(b), RSC_OK);

  // the first frame of A: a leading 0x00, then SYNC (version 2, nothing in flight, MTU 256)
  rscService(&a.link);
  CHECK(a.capture == hexBytes("00 01 03 02 02 01 08 01 78 56 34 12 53 D2 00"));
  a.capturing = false;
  a.capture.clear();
  CHECK(connect(a, b, w));

  // 5 events from A and 3 from B, all acknowledged: A sends SEQ 5 and expects 3
  for (int i = 0; i < 5; i++) sendRetry(a, b, w, 0x20, Bytes());
  for (int i = 0; i < 3; i++) sendRetry(b, a, w, 0x20, Bytes());
  pumpUntil(a, b, w, 2000, [&]() { return rscQueued(&a.link) == 0 && rscQueued(&b.link) == 0; });
  pump(a, b, w, 100);     // idle: the next frame starts with 0x00

  // DATA, reliable, SEQ 5, ACK 3, type 0x10, request tag 1, data 01 00 02
  a.capturing = true;
  const uint8_t d[3] = { 0x01, 0x00, 0x02 };
  uint8_t tag = 0;
  CHECK_EQ(rscRequest(&a.link, 0x10, d, 3, 1000, onResponse, &a, &tag), RSC_OK);
  CHECK_EQ(tag, 1);
  rscService(&a.link);
  CHECK(a.capture == hexBytes("00 02 5D 04 10 01 01 04 02 CF 94 00"));

  // B acknowledges with a pure ACK 6 (type 0x10 gets no response)
  b.capturing = true;
  deliver(a, b, w);
  rscService(&b.link);
  CHECK(b.capture == hexBytes("00 05 30 01 BB 08 00"));
#endif
}

static void test_connect() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  CHECK_EQ(initPeer(a), RSC_OK);
  CHECK_EQ(initPeer(b), RSC_OK);
  CHECK(!rscIsUp(&a.link));
  CHECK(connect(a, b, w));
  pump(a, b, w, 500);
  CHECK_EQ(countEvent(a, RSC_EVENT_UP), 1);
  CHECK_EQ(countEvent(b, RSC_EVENT_UP), 1);
  CHECK_EQ(countEvent(a, RSC_EVENT_PEER_RESET), 0);
  CHECK_EQ(a.lockDepth, 0);
  CHECK(a.maxLockDepth <= 1);
}

// One side starts long after the other
static void test_connect_late_peer() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  CHECK_EQ(initPeer(a), RSC_OK);
  for (int i = 0; i < 2000; i++) {
    rscService(&a.link);
    a.out.clear();
    simNow++;
  }
  sendRetry(a, b, w, 0x21, hexBytes("AA"));      // queued while nobody listens
  CHECK_EQ(initPeer(b), RSC_OK);
  CHECK(connect(a, b, w));
  pumpUntil(a, b, w, 1000, [&]() { return !b.msgs.empty(); });
  CHECK_EQ(b.msgs.size(), 1u);
  if ( !b.msgs.empty() ) CHECK(b.msgs[0].data == hexBytes("AA"));
}

static void test_requests() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  uint8_t t1 = requestRetry(a, b, w, 0x01, hexBytes("01 02"), 1000);
  uint8_t t2 = requestRetry(a, b, w, 0x02, Bytes(), 1000);
  pumpUntil(a, b, w, 1000, [&]() { return findRsp(a, t1) && findRsp(a, t2); });
  uint8_t t3 = requestRetry(a, b, w, 0x03, hexBytes("09 08 07"), 1000);
  pumpUntil(a, b, w, 1000, [&]() { return findRsp(a, t3) != NULL; });

  const Rsp* r = findRsp(a, t1);
  CHECK(r != NULL);
  if ( r ) {
    CHECK_EQ(r->event, RSC_RESPONSE_ACK);
    CHECK_EQ(r->type, RSC_TYPE_ACK);
    CHECK(r->data == hexBytes("01 02"));
  }
  r = findRsp(a, t2);
  CHECK(r != NULL);
  if ( r ) {
    CHECK_EQ(r->event, RSC_RESPONSE_NACK);
    CHECK_EQ(r->error, RSC_ERROR_CMD_BAD_ARG);
    CHECK(r->data == hexBytes("6E 6F"));
  }
  r = findRsp(a, t3);
  CHECK(r != NULL);
  if ( r ) {
    CHECK_EQ(r->event, RSC_RESPONSE_DATA);
    CHECK_EQ(r->type, 0x33);
    CHECK(r->data == hexBytes("09 08 07"));
  }
  CHECK_EQ(a.rsps.size(), 3u);
  CHECK(t1 != t2 && t2 != t3);
}

// Without PROGRESS a slow command times out
static void test_progress_timeout() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  uint8_t t = requestRetry(a, b, w, 0x04, Bytes(), 200);     // done after 300 ms
  pumpUntil(a, b, w, 2000, [&]() { return findRsp(a, t) != NULL; });
  pump(a, b, w, 300);
  CHECK_EQ(a.rsps.size(), 2u);
  if ( a.rsps.size() == 2 ) {
    CHECK_EQ(a.rsps[0].event, RSC_RESPONSE_PROGRESS);
    CHECK_EQ(a.rsps[1].event, RSC_RESPONSE_TIMEOUT);
  }
#if RSC_STATS
  CHECK_EQ(rscGetStats(&a.link)->lateResponses, 1);     // the ACK came too late
#endif
}

// Each PROGRESS restarts the timeout: done at 400 ms with a 250 ms timeout
static void test_progress_restarts_timeout() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  uint8_t t = requestRetry(a, b, w, 0x06, Bytes(), 250);
  pumpUntil(a, b, w, 2000, [&]() { return findRsp(a, t) != NULL; });
  CHECK_EQ(a.rsps.size(), 3u);
  if ( a.rsps.size() == 3 ) {
    CHECK_EQ(a.rsps[0].event, RSC_RESPONSE_PROGRESS);
    CHECK_EQ(a.rsps[1].event, RSC_RESPONSE_PROGRESS);
    CHECK(a.rsps[1].data == hexBytes("32"));
    CHECK_EQ(a.rsps[2].event, RSC_RESPONSE_ACK);
  }
}

static void test_progress_then_final() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  uint8_t t = requestRetry(a, b, w, 0x04, Bytes(), 1000);
  pumpUntil(a, b, w, 2000, [&]() { return findRsp(a, t) != NULL; });
  CHECK_EQ(a.rsps.size(), 2u);
  if ( a.rsps.size() == 2 ) {
    CHECK_EQ(a.rsps[0].event, RSC_RESPONSE_PROGRESS);
    CHECK_EQ(a.rsps[1].event, RSC_RESPONSE_ACK);
    CHECK(a.rsps[1].data == hexBytes("64 6F 6E 65"));
  }
}

static void test_timeout_and_void() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  // no response at all
  uint8_t t = requestRetry(a, b, w, 0x05, Bytes(), 100);
  pump(a, b, w, 150);
  const Rsp* r = findRsp(a, t);
  CHECK(r != NULL && r->event == RSC_RESPONSE_TIMEOUT);
  CHECK_EQ(b.msgs.size(), 1u);

  // a request that times out before it leaves is withdrawn: B never sees it. An idle link
  // does not notice a cut line, so an event in flight takes the link down first.
  w.cut = true;
  sendRetry(a, b, w, 0x22, hexBytes("01"));
  pumpUntil(a, b, w, 10000, [&]() { return !rscIsUp(&a.link); });
  CHECK_EQ(countEvent(a, RSC_EVENT_DOWN), 1);
  b.msgs.clear();
  uint8_t t2 = 0;
  CHECK_EQ(rscRequest(&a.link, 0x01, NULL, 0, 50, onResponse, &a, &t2), RSC_OK);
  pump(a, b, w, 100);
  r = findRsp(a, t2);
  CHECK(r != NULL && r->event == RSC_RESPONSE_TIMEOUT);
  w.cut = false;
  CHECK(connect(a, b, w));
  pump(a, b, w, 300);
  CHECK_EQ(b.msgs.size(), 1u);          // the event only
  if ( !b.msgs.empty() ) CHECK_EQ(b.msgs[0].type, 0x22);
  CHECK_EQ(rscQueued(&a.link), 0);
}

static void test_arguments() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  rscConfig_t cfg;
  rscOps_t ops;
  rscConfigDefaults(&cfg);
  memset(&ops, 0, sizeof(ops));
  CHECK_EQ(rscInit(NULL, &cfg, &ops), RSC_ERROR_NULL);
  CHECK_EQ(rscInit(&a.link, &cfg, &ops), RSC_ERROR_NULL);    // no write, no millis
  ops.write = opWrite;
  ops.millis = opMillis;
  ops.lock = opLock;                                         // lock without unlock
  CHECK_EQ(rscInit(&a.link, &cfg, &ops), RSC_ERROR_BAD_ARG);
  ops.lock = NULL;
  cfg.window = 8;
  CHECK_EQ(rscInit(&a.link, &cfg, &ops), RSC_ERROR_BAD_ARG);
  cfg.window = 0;
  cfg.mtu = RSC_MTU + 1;
  CHECK_EQ(rscInit(&a.link, &cfg, &ops), RSC_ERROR_BAD_ARG);

  initPeer(a);
  initPeer(b);
  uint8_t big[RSC_MTU + 1];
  memset(big, 1, sizeof(big));
  CHECK_EQ(rscSend(&a.link, 0xF0, NULL, 0), RSC_ERROR_BAD_ARG);
  CHECK_EQ(rscSend(&a.link, 0x01, NULL, 3), RSC_ERROR_NULL);
  CHECK_EQ(rscSend(&a.link, 0x01, big, RSC_MTU + 1), RSC_ERROR_OVERFLOW);
  CHECK_EQ(rscSend(&a.link, 0x01, big, RSC_MTU), RSC_OK);
  CHECK_EQ(rscRequest(&a.link, 0xFF, NULL, 0, 100, onResponse, &a, NULL), RSC_ERROR_BAD_ARG);
  CHECK_EQ(rscRespond(&a.link, 0, 0x01, NULL, 0), RSC_ERROR_BAD_ARG);
  CHECK_EQ(rscRespond(&a.link, 128, 0x01, NULL, 0), RSC_ERROR_BAD_ARG);
  CHECK_EQ(rscRespond(&a.link, 5, 0xFE, NULL, 0), RSC_ERROR_BAD_ARG);
  CHECK_EQ(rscNack(&a.link, 5, -1, big, RSC_MTU), RSC_ERROR_OVERFLOW);   // error byte + data
  CHECK_EQ(rscSend(NULL, 0x01, NULL, 0), RSC_ERROR_NULL);

  // queue full
  initPeer(a);
  for (int i = 0; i < RSC_TX_SLOTS; i++) CHECK_EQ(rscSend(&a.link, 0x01, NULL, 0), RSC_OK);
  CHECK_EQ(rscSend(&a.link, 0x01, NULL, 0), RSC_ERROR_QUEUE_FULL);
  CHECK_EQ(rscQueued(&a.link), RSC_TX_SLOTS);

  // no free tag
  initPeer(a);
  int ok = 0;
  for (int i = 0; i < RSC_MAX_REQUESTS + 1 && i < RSC_TX_SLOTS; i++) {
    if ( rscRequest(&a.link, 0x01, NULL, 0, 0, onResponse, &a, NULL) == RSC_OK ) ok++;
  }
  if ( RSC_TX_SLOTS > RSC_MAX_REQUESTS ) {
    CHECK_EQ(ok, RSC_MAX_REQUESTS);
    CHECK_EQ(rscRequest(&a.link, 0x01, NULL, 0, 0, onResponse, &a, NULL), RSC_ERROR_NO_TAG);
  }
}

// Every event arrives once and in order, and every request is answered once, on a noisy line
static void lossyRun(double drop, double flip, double insert, uint32_t seed, int events, int requests, uint8_t window) {
  Peer a("A", seed), b("B", seed + 100);
  Wire w;
  w.drop = drop;
  w.flip = flip;
  w.insert = insert;
  w.rng.seed(seed);
  initPeer(a, 0, window);
  initPeer(b, 0, window);
  CHECK(connect(a, b, w));

  std::vector<uint8_t> tags;
  int sent = 0, asked = 0;
  while ( sent < events || asked < requests ) {
    if ( sent < events ) {
      Bytes d;
      d.push_back((uint8_t) sent);
      d.push_back((uint8_t) (sent >> 8));
      sendRetry(a, b, w, 0x22, d);
      sent++;
    }
    if ( asked < requests && sent % 3 == 0 ) {
      Bytes d;
      d.push_back((uint8_t) asked);
      tags.push_back(requestRetry(a, b, w, 0x01, d, 60000));
      asked++;
    }
  }
  pumpUntil(a, b, w, 600000, [&]() {
    return rscQueued(&a.link) == 0 && rscQueued(&b.link) == 0 && a.rsps.size() >= (size_t) requests;
  });

  // events: exactly once, in order
  int next = 0;
  bool ordered = true;
  for (size_t i = 0; i < b.msgs.size(); i++) {
    if ( b.msgs[i].type != 0x22 ) continue;
    int v = b.msgs[i].data[0] | (b.msgs[i].data[1] << 8);
    if ( v != next ) ordered = false;
    next++;
  }
  CHECK(ordered);
  CHECK_EQ(next, events);

  // requests: every one answered with its own data, once
  CHECK_EQ(a.rsps.size(), (size_t) requests);
  for (size_t i = 0; i < a.rsps.size(); i++) CHECK_EQ(a.rsps[i].event, RSC_RESPONSE_ACK);
  CHECK_EQ(a.lockDepth, 0);
#if RSC_STATS
  if ( drop + flip + insert > 0 ) {
    const rscStats_t* s = rscGetStats(&a.link);
    CHECK(s->retransmits > 0);
  }
#endif
}

static void test_lossy_link() {
  lossyRun(0.0, 0.0, 0.0, 11, 300, 60, 0);
  lossyRun(0.01, 0.0, 0.0, 12, 300, 60, 0);
  lossyRun(0.0, 0.01, 0.0, 13, 300, 60, 0);
  lossyRun(0.0, 0.0, 0.01, 14, 300, 60, 0);
  lossyRun(0.02, 0.02, 0.01, 15, 300, 60, 0);
  lossyRun(0.02, 0.02, 0.01, 16, 200, 40, 1);
  if ( RSC_TX_SLOTS >= 7 ) lossyRun(0.02, 0.02, 0.01, 17, 300, 60, 7);
}

// Up to the window of frames leaves before any ACK
static void test_window() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));
  pump(a, b, w, 50);

  int n = RSC_TX_SLOTS < RSC_WINDOW ? RSC_TX_SLOTS : RSC_WINDOW;
  for (int i = 0; i < RSC_TX_SLOTS; i++) rscSend(&a.link, 0x22, NULL, 0);
  a.out.clear();
  rscService(&a.link);
  int frames = 0;
  for (size_t i = 0; i < a.out.size(); i++) {
    if ( a.out[i] == 0 && i > 0 && a.out[i - 1] != 0 ) frames++;
  }
  CHECK_EQ(frames, n);
  CHECK_EQ(a.link.inFlight, n);
}

// A refused message pauses the sender until rscResume()
static void test_not_ready() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  bool refuse = false;
  b.handler = [&](Peer& p, const rscMessage_t* m) -> bool {
    if ( refuse && m->data[0] == 3 ) return false;
    return defaultHandler(p, m);
  };
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  // 0, 1, 2 are delivered, 3 is refused, 4 waits behind it (the queue may hold only 2)
  refuse = true;
  for (int i = 0; i < 5; i++) {
    Bytes d(1, (uint8_t) i);
    sendRetry(a, b, w, 0x22, d);
  }
  pump(a, b, w, 3000);
  CHECK_EQ(b.msgs.size(), 3u);          // 0, 1, 2 delivered; 3 refused
  CHECK(rscIsUp(&a.link));              // probing does not take the link down
  CHECK(a.link.peerRnr);

  refuse = false;
  rscResume(&b.link);
  sendRetry(a, b, w, 0x22, Bytes(1, 5));
  pumpUntil(a, b, w, 1000, [&]() { return b.msgs.size() == 6; });
  CHECK_EQ(b.msgs.size(), 6u);
  for (size_t i = 0; i < b.msgs.size(); i++) CHECK_EQ(b.msgs[i].data[0], (int) i);
}

// The line breaks: the link goes down, keeps its queue, and comes back
static void test_link_down_up() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  w.cut = true;
  for (int i = 0; i < RSC_TX_SLOTS; i++) {
    Bytes d(1, (uint8_t) i);
    CHECK_EQ(rscSend(&a.link, 0x22, d.data(), 1), RSC_OK);
  }
  pumpUntil(a, b, w, 10000, [&]() { return !rscIsUp(&a.link); });
  CHECK_EQ(countEvent(a, RSC_EVENT_DOWN), 1);
  pump(a, b, w, 2000);
  w.cut = false;
  CHECK(connect(a, b, w));
  pumpUntil(a, b, w, 2000, [&]() { return rscQueued(&a.link) == 0; });
  CHECK_EQ(b.msgs.size(), (size_t) RSC_TX_SLOTS);
  for (size_t i = 0; i < b.msgs.size(); i++) CHECK_EQ(b.msgs[i].data[0], (int) i);
  CHECK_EQ(countEvent(a, RSC_EVENT_PEER_RESET), 0);
  CHECK_EQ(countEvent(b, RSC_EVENT_PEER_RESET), 0);
}

// B restarts: A sees PEER_RESET, fails the request B had received, and sends the rest to the new B
static void peerResetRun(bool sameNonce) {
  Peer a("A", 1), b("B", 2);
  Wire w;
  if ( sameNonce ) b.nonce = 0x55AA55AA;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  // traffic both ways, so that the sequence numbers are not at 0
  for (int i = 0; i < 3; i++) sendRetry(a, b, w, 0x22, Bytes(1, 0x10));
  for (int i = 0; i < 5; i++) sendRetry(b, a, w, 0x22, Bytes(1, 0x10));
  pumpUntil(a, b, w, 2000, [&]() { return rscQueued(&a.link) == 0 && rscQueued(&b.link) == 0; });

  // a request that B receives and never answers
  uint8_t t = requestRetry(a, b, w, 0x05, Bytes(), 0);
  pumpUntil(a, b, w, 2000, [&]() { return rscQueued(&a.link) == 0; });
  CHECK(findRsp(a, t) == NULL);

  // B restarts; A queues more while B is gone
  w.cut = true;
  b.msgs.clear();
  b.events.clear();
  initPeer(b);
  sendRetry(a, b, w, 0x22, hexBytes("77"));
  w.cut = false;
  CHECK(connect(a, b, w));
  pumpUntil(a, b, w, 3000, [&]() { return !b.msgs.empty(); });

  if ( !sameNonce ) CHECK_EQ(countEvent(a, RSC_EVENT_PEER_RESET), 1);
  CHECK(countEvent(a, RSC_EVENT_PEER_RESET) >= 1);
  const Rsp* r = findRsp(a, t);
  CHECK(r != NULL && r->event == RSC_RESPONSE_PEER_RESET);
  CHECK_EQ(b.msgs.size(), 1u);
  if ( !b.msgs.empty() ) CHECK(b.msgs[0].data == hexBytes("77"));

  // and traffic flows both ways again
  uint8_t t2 = requestRetry(a, b, w, 0x01, hexBytes("42"), 1000);
  sendRetry(b, a, w, 0x23, hexBytes("43"));
  pumpUntil(a, b, w, 2000, [&]() { return findRsp(a, t2) != NULL; });
  r = findRsp(a, t2);
  CHECK(r != NULL && r->event == RSC_RESPONSE_ACK);
  pump(a, b, w, 200);
  bool got = false;
  for (size_t i = 0; i < a.msgs.size(); i++) if ( a.msgs[i].type == 0x23 ) got = true;
  CHECK(got);
}

static void test_peer_reset() {
  peerResetRun(false);
}

// A restarted peer that draws the same nonce is still detected by its sequence state
static void test_peer_reset_same_nonce() {
  peerResetRun(true);
}

// The smaller MTU wins. A request queued before the sync and longer than the peer's MTU is dropped.
static void test_mtu() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b, 16);
  Bytes big(32, 0x5A);
  uint8_t t = 0;
  CHECK_EQ(rscRequest(&a.link, 0x01, big.data(), 32, 1000, onResponse, &a, &t), RSC_OK);
  sendRetry(a, b, w, 0x22, hexBytes("01"));
  CHECK(connect(a, b, w));
  CHECK_EQ(rscMtu(&a.link), 16);
  CHECK_EQ(rscMtu(&b.link), 16);
  pumpUntil(a, b, w, 1000, [&]() { return rscQueued(&a.link) == 0; });
  const Rsp* r = findRsp(a, t);
  CHECK(r != NULL && r->event == RSC_RESPONSE_DROPPED);
  CHECK_EQ(b.msgs.size(), 1u);
  CHECK_EQ(rscSend(&a.link, 0x22, big.data(), 17), RSC_ERROR_OVERFLOW);
  CHECK_EQ(rscSend(&a.link, 0x22, big.data(), 16), RSC_OK);
}

static void test_one_byte_at_a_time() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  w.oneByte = true;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));
  uint8_t t = requestRetry(a, b, w, 0x03, hexBytes("01 00 00 02"), 5000);
  pumpUntil(a, b, w, 5000, [&]() { return findRsp(a, t) != NULL; });
  const Rsp* r = findRsp(a, t);
  CHECK(r != NULL && r->event == RSC_RESPONSE_DATA && r->data == hexBytes("01 00 00 02"));
}

// Timers keep working across the wrap of the millisecond counter
static void test_clock_wrap() {
  simNow = 0xFFFFFF00u;
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));
  w.drop = 0.05;
  for (int i = 0; i < 20; i++) requestRetry(a, b, w, 0x01, Bytes(1, (uint8_t) i), 20000);
  pumpUntil(a, b, w, 60000, [&]() { return a.rsps.size() == 20; });
  CHECK_EQ(a.rsps.size(), 20u);
  CHECK(simNow < 0x80000000u);
  simNow = 1000;
}

// Garbage, damaged frames and reserved bits never crash the link and never reach the application
static void test_malformed() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));

  std::mt19937 rng(99);
  for (int i = 0; i < 20000; i++) {
    uint8_t buf[64];
    size_t n = rng() % sizeof(buf);
    for (size_t k = 0; k < n; k++) buf[k] = (uint8_t) rng();
    rscFeed(&a.link, buf, n);
  }
  // a valid frame with a reserved bit, and one with the "more" bit (after a delimiter that
  // ends the garbage)
  uint8_t f[8];
  uint8_t enc[16];
  uint8_t delim = 0;
  rscFeed(&a.link, &delim, 1);
  f[0] = 0x40; f[1] = 0x20; f[2] = 0x01; f[3] = 0x00;
  uint16_t c = rscCrc16(f, 4, RSC_CRC16_INIT);
  f[4] = (uint8_t) c; f[5] = (uint8_t) (c >> 8);
  size_t n = rscCobsEncode(f, 6, enc);
  enc[n++] = 0;
  rscFeed(&a.link, enc, n);
  f[1] = RSC_L1_MORE;
  c = rscCrc16(f, 4, RSC_CRC16_INIT);
  f[4] = (uint8_t) c; f[5] = (uint8_t) (c >> 8);
  n = rscCobsEncode(f, 6, enc);
  enc[n++] = 0;
  rscFeed(&a.link, enc, n);

  // the link survives (a random frame may have looked like a peer reset)
  a.msgs.clear();
  uint8_t zero = 0;
  rscFeed(&a.link, &zero, 1);
  CHECK(connect(a, b, w));
  uint8_t t = requestRetry(b, a, w, 0x01, hexBytes("AB"), 5000);
  pumpUntil(a, b, w, 5000, [&]() { return findRsp(b, t) != NULL; });
  CHECK(findRsp(b, t) != NULL && findRsp(b, t)->event == RSC_RESPONSE_ACK);
#if RSC_STATS
  const rscStats_t* s = rscGetStats(&a.link);
  CHECK(s->framingErrors + s->crcErrors > 1000);
  CHECK(s->rejected >= 2);
#endif
}

// Responses without a pending request are counted and dropped
static void test_late_response() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));
  CHECK_EQ(rscAck(&b.link, 77, NULL, 0), RSC_OK);
  pump(a, b, w, 200);
  CHECK_EQ(a.rsps.size(), 0u);
#if RSC_STATS
  CHECK_EQ(rscGetStats(&a.link)->lateResponses, 1);
#endif
}

// A failing write does not break the link: the frames go again
static void test_write_errors() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a);
  initPeer(b);
  CHECK(connect(a, b, w));
  a.writeFails = true;
  sendRetry(a, b, w, 0x22, hexBytes("99"));
  pump(a, b, w, 20);
  a.writeFails = false;
  pumpUntil(a, b, w, 3000, [&]() { return !b.msgs.empty(); });
  CHECK_EQ(b.msgs.size(), 1u);
#if RSC_STATS
  CHECK(rscGetStats(&a.link)->txErrors > 0);
#endif
}

// The link works without lock operations (a single-threaded superloop)
static void test_no_lock() {
  Peer a("A", 1), b("B", 2);
  Wire w;
  initPeer(a, 0, 0, false);
  initPeer(b, 0, 0, false);
  CHECK(connect(a, b, w));
  uint8_t t = requestRetry(a, b, w, 0x01, hexBytes("10"), 1000);
  pumpUntil(a, b, w, 1000, [&]() { return findRsp(a, t) != NULL; });
  CHECK(findRsp(a, t) != NULL);
}


// Runner =====================================================================
struct TestCase {
  const char* name;
  void (*fn)();
};

static const TestCase tests[] = {
  { "crc_check_value", test_crc_check_value },
  { "cobs_vectors", test_cobs_vectors },
  { "golden_frames", test_golden_frames },
  { "connect", test_connect },
  { "connect_late_peer", test_connect_late_peer },
  { "requests", test_requests },
  { "progress_timeout", test_progress_timeout },
  { "progress_restarts_timeout", test_progress_restarts_timeout },
  { "progress_then_final", test_progress_then_final },
  { "timeout_and_void", test_timeout_and_void },
  { "arguments", test_arguments },
  { "lossy_link", test_lossy_link },
  { "window", test_window },
  { "not_ready", test_not_ready },
  { "link_down_up", test_link_down_up },
  { "peer_reset", test_peer_reset },
  { "peer_reset_same_nonce", test_peer_reset_same_nonce },
  { "mtu", test_mtu },
  { "one_byte_at_a_time", test_one_byte_at_a_time },
  { "clock_wrap", test_clock_wrap },
  { "malformed", test_malformed },
  { "late_response", test_late_response },
  { "write_errors", test_write_errors },
  { "no_lock", test_no_lock },
};

int main(int argc, char** argv) {
  int run = 0;
  printf("ReliableSerialComms %s host tests (MTU %d, %d slots, window %d, %d requests, CRC impl %d, stats %d)\n",
         RSC_VERSION_STRING, RSC_MTU, RSC_TX_SLOTS, RSC_WINDOW, RSC_MAX_REQUESTS, RSC_CRC_IMPL, RSC_STATS);
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if ( argc > 1 && strcmp(argv[1], tests[i].name) != 0 ) continue;
    int before = failures;
    simNow = 1000;
    tests[i].fn();
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    run++;
  }
  printf("%d tests, %d failures\n", run, failures);
  return ( failures == 0 && run > 0 ) ? 0 : 1;
}
