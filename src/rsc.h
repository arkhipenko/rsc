#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rsc_cobs.h"
#include "rsc_crc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Version information
#define RSC_VERSION_MAJOR     2
#define RSC_VERSION_MINOR     0
#define RSC_VERSION_RELEASE   0
#define RSC_VERSION           20000     // major * 10000 + minor * 100 + patch
#define RSC_VERSION_STRING    "2.0.0"

// Wire protocol version carried in SYNC frames. Both peers must use the same one.
#define RSC_PROTOCOL_VERSION  2


// Build options ===============================================================
// Every link has the same buffer sizes. AVR gets smaller defaults.

// Largest data length of one message, in bytes (1-4089). Peers use the smaller of their two values.
#ifndef RSC_MTU
#if defined(__AVR__)
#define RSC_MTU               64
#else
#define RSC_MTU               256
#endif
#endif

// Messages held for transmission per link, including the frames in flight
#ifndef RSC_TX_SLOTS
#if defined(__AVR__)
#define RSC_TX_SLOTS          2
#else
#define RSC_TX_SLOTS          8
#endif
#endif

// Default number of frames in flight (1-7). rscConfig_t.window overrides it per link.
#ifndef RSC_WINDOW
#if defined(__AVR__)
#define RSC_WINDOW            1
#else
#define RSC_WINDOW            4
#endif
#endif

// Requests waiting for their final response, per link (1-127)
#ifndef RSC_MAX_REQUESTS
#if defined(__AVR__)
#define RSC_MAX_REQUESTS      2
#else
#define RSC_MAX_REQUESTS      8
#endif
#endif

// Counters per link (rscGetStats)
#ifndef RSC_STATS
#if defined(__AVR__)
#define RSC_STATS             0
#else
#define RSC_STATS             1
#endif
#endif

// Retransmission timeouts without progress before the link is down
#ifndef RSC_RETRIES
#define RSC_RETRIES           6
#endif

// Retransmission timeout: RSC_RTO_MIN_MS + 3 x the time of a maximum frame at the configured
// baud, or RSC_RTO_DEFAULT_MS when the baud is unknown. It doubles after each expiry, up to
// RSC_RTO_MAX_FACTOR times the first value.
#ifndef RSC_RTO_MIN_MS
#define RSC_RTO_MIN_MS        50
#endif
#ifndef RSC_RTO_DEFAULT_MS
#define RSC_RTO_DEFAULT_MS    200
#endif
#ifndef RSC_RTO_MAX_FACTOR
#define RSC_RTO_MAX_FACTOR    8
#endif

// Interval of SYNC frames while the link is not confirmed, in ms
#ifndef RSC_SYNC_INTERVAL_MS
#define RSC_SYNC_INTERVAL_MS  250
#endif

// A frame sent after this many ms of silence starts with an extra 0x00, which makes the
// receiver discard a partial frame left by line noise
#ifndef RSC_FLUSH_IDLE_MS
#define RSC_FLUSH_IDLE_MS     20
#endif

// Longest wait rscService() returns when nothing is scheduled, in ms
#ifndef RSC_IDLE_MS
#define RSC_IDLE_MS           1000
#endif

#if RSC_MTU < 1 || RSC_MTU > 4089
#error "RSC_MTU must be 1 to 4089 (a frame must stay within 32751 bits for CRC-16 Hamming distance 4)"
#endif
#if RSC_TX_SLOTS < 1 || RSC_TX_SLOTS > 255
#error "RSC_TX_SLOTS must be 1 to 255"
#endif
#if RSC_WINDOW < 1 || RSC_WINDOW > 7
#error "RSC_WINDOW must be 1 to 7"
#endif
#if RSC_MAX_REQUESTS < 1 || RSC_MAX_REQUESTS > 127
#error "RSC_MAX_REQUESTS must be 1 to 127"
#endif


// Wire constants ==============================================================
#define RSC_LINK_HEADER_LEN   2       // link byte 0 and link byte 1
#define RSC_MESSAGE_HEADER_LEN 2      // message type and tag byte
#define RSC_CRC_LEN           2
#define RSC_MAX_WINDOW        7       // sequence numbers are 3 bits

// Link byte 0
#define RSC_L0_SEQ_MASK       0x07
#define RSC_L0_ACK_SHIFT      3
#define RSC_L0_RELIABLE       0x40
#define RSC_L0_RNR            0x80    // receiver not ready

// Link byte 1
#define RSC_KIND_DATA         0
#define RSC_KIND_ACK          1
#define RSC_KIND_SYNC         2
#define RSC_KIND_SYNC_RSP     3
#define RSC_L1_KIND_MASK      0x0F
#define RSC_L1_MORE           0x10    // another segment follows (reserved for 2.1.0)
#define RSC_L1_RESERVED       0xE0

// SYNC body: version, frames in flight, MTU (2), nonce (4). SYNC RESPONSE adds the nonce it answers (4).
#define RSC_SYNC_LEN          8
#define RSC_SYNC_RSP_LEN      12

// Tag byte
#define RSC_TAG_RESPONSE      0x80
#define RSC_TAG_MASK          0x7F

// Message types. 0x00-0xEF belong to the application.
#define RSC_TYPE_APP_MAX      0xEF
#define RSC_TYPE_VOID         0xF0    // a frame withdrawn by its sender; acknowledged and discarded
#define RSC_TYPE_PROGRESS     0xFD    // interim response: the request is accepted, the result follows
#define RSC_TYPE_NACK         0xFE    // final response: failure, the first data byte is a signed error code
#define RSC_TYPE_ACK          0xFF    // final response: success, data optional


// Error codes =================================================================
typedef int16_t               rscError_t;

#define RSC_OK                0
#define RSC_ERROR_GENERAL     (-1)
#define RSC_ERROR_TIMEOUT     (-2)
#define RSC_ERROR_OVERFLOW    (-3)    // data longer than the MTU
#define RSC_ERROR_NULL        (-4)
#define RSC_ERROR_QUEUE_FULL  (-5)    // RSC_TX_SLOTS messages already queued
#define RSC_ERROR_NO_TAG      (-6)    // RSC_MAX_REQUESTS requests already pending
#define RSC_ERROR_BAD_ARG     (-7)    // reserved message type, or a tag outside 1-127

// Suggested NACK error codes (any value from -1 to -128 may be used)
#define RSC_ERROR_CMD_BAD_ARG (-60)
#define RSC_ERROR_CMD_NOT_IMP (-61)
#define RSC_ERROR_CMD_BUSY    (-62)


// Callback types ==============================================================
typedef struct rscLink_s rscLink_t;

// A request (tag 1-127) or an event (tag 0) from the peer. data is valid only during the callback.
typedef struct {
  uint8_t         type;     // message type, 0x00-0xEF
  uint8_t         tag;      // request tag; 0 when no response is expected
  const uint8_t*  data;
  uint16_t        length;
} rscMessage_t;

// What happened to a request
#define RSC_RESPONSE_DATA       0   // final: a response with an application type and its data
#define RSC_RESPONSE_ACK        1   // final: ACK, data optional
#define RSC_RESPONSE_NACK       2   // final: NACK, error holds the peer's error code
#define RSC_RESPONSE_PROGRESS   3   // interim: the peer accepted the request; the timeout restarts
#define RSC_RESPONSE_TIMEOUT    4   // final: no final response in time
#define RSC_RESPONSE_PEER_RESET 5   // final: the peer restarted after it had received the request
#define RSC_RESPONSE_DROPPED    6   // final: the request is longer than the peer's MTU; it was not sent

typedef struct {
  uint8_t         tag;      // tag of the request
  uint8_t         event;    // RSC_RESPONSE_*
  uint8_t         type;     // message type of the response frame; 0 when none arrived
  int8_t          error;    // NACK: the peer's error code (RSC_ERROR_GENERAL when it sent none)
  const uint8_t*  data;     // response data (for a NACK, the bytes after the error code)
  uint16_t        length;
} rscResponse_t;

// Link events
#define RSC_EVENT_UP          1     // the link is synchronized; queued messages flow
#define RSC_EVENT_DOWN        2     // retransmissions failed; the queue is kept and the link syncs again
#define RSC_EVENT_PEER_RESET  3     // the peer restarted; the link syncs again

// Return false to refuse the message: the library then marks this side "not ready", the peer
// pauses, and the message comes again after rscResume().
typedef bool (*rscMessageCb_t)( rscLink_t* link, const rscMessage_t* msg, void* user );
typedef void (*rscResponseCb_t)( rscLink_t* link, const rscResponse_t* rsp, void* user );
typedef void (*rscEventCb_t)( rscLink_t* link, uint8_t event, void* user );


// Platform operations (one set per link) ======================================
typedef struct {
  // Writes bytes to the bus. Returns length, or a negative error. May block until the bytes fit.
  int       (*write)( void* ctx, const uint8_t* data, size_t length );
  // Milliseconds since start. May wrap.
  uint32_t  (*millis)( void* ctx );
  // Optional. A random 32-bit value for the session nonce. The peer detects a restart of this
  // side by a new nonce, so the value should differ on every start.
  uint32_t  (*random)( void* ctx );
  // Optional. Required when another thread queues messages while rscFeed() or rscService() runs.
  void      (*lock)( void* ctx );
  void      (*unlock)( void* ctx );
  // Optional. Called after a message is queued, to wake the thread that runs rscService().
  void      (*wake)( void* ctx );
  void*     ctx;
} rscOps_t;

// Link settings. rscConfigDefaults() fills them; 0 means "use the build default".
typedef struct {
  uint32_t        baud;       // bit rate of the line, for the retransmission timeout; 0 when unknown
  uint16_t        rtoMs;      // first retransmission timeout; 0 derives it from baud
  uint16_t        mtu;        // largest data length, 1 to RSC_MTU
  uint8_t         window;     // frames in flight, 1 to 7
  uint8_t         retries;    // retransmission timeouts without progress before the link is down
  rscMessageCb_t  onMessage;  // requests and events from the peer
  rscEventCb_t    onEvent;    // link events (optional)
  void*           user;       // passed to onMessage and onEvent
} rscConfig_t;

typedef struct {
  uint32_t  txFrames;         // frames written (DATA, ACK, SYNC)
  uint32_t  rxFrames;         // frames that passed the CRC
  uint32_t  txErrors;         // writes that failed or were short
  uint32_t  crcErrors;
  uint32_t  framingErrors;    // malformed COBS, oversize or short frames
  uint32_t  rejected;         // reserved bits or kinds, wrong protocol version, malformed bodies
  uint32_t  retransmits;      // DATA frames sent again
  uint32_t  duplicates;       // DATA frames out of sequence (discarded and acknowledged)
  uint32_t  timeouts;         // retransmission timer expiries
  uint32_t  linkDowns;
  uint32_t  peerResets;
  uint32_t  lateResponses;    // responses without a pending request
  uint32_t  dropped;          // messages longer than the peer's MTU
  uint32_t  busy;             // DATA frames discarded while this side was not ready
} rscStats_t;


// Link state (allocate it statically; the fields are private) =================
#define RSC_FRAME_MAX         (RSC_LINK_HEADER_LEN + RSC_MESSAGE_HEADER_LEN + RSC_MTU + RSC_CRC_LEN)
#define RSC_RX_MAX            ( RSC_FRAME_MAX > (RSC_LINK_HEADER_LEN + RSC_SYNC_RSP_LEN + RSC_CRC_LEN) ? \
                                RSC_FRAME_MAX : (RSC_LINK_HEADER_LEN + RSC_SYNC_RSP_LEN + RSC_CRC_LEN) )

typedef struct {
  uint16_t        length;               // message bytes: type, tag byte and data
  uint8_t         frame[RSC_FRAME_MAX]; // link header, message, room for the CRC
} rscSlot_t;

typedef struct {
  rscResponseCb_t callback;
  void*           user;
  uint32_t        start;                // queued, or the last PROGRESS
  uint32_t        timeout;
  uint8_t         tag;                  // 0 when the entry is free
  uint8_t         delivered;            // the peer acknowledged the request frame
} rscPending_t;

struct rscLink_s {
  rscOps_t          ops;
  rscConfig_t       cfg;
  uint32_t          rto0;               // first retransmission timeout
  uint16_t          peerMtu;

  // session
  uint32_t          nonce;
  uint32_t          peerNonce;
  uint32_t          syncEcho;           // nonce to answer with the next SYNC RESPONSE
  uint32_t          lastSync;
  bool              peerKnown;
  bool              confirmed;
  bool              up;
  bool              syncDue;
  bool              syncRspDue;

  // transmit: slots[head] holds sequence number base; inFlight frames have been sent
  rscSlot_t         slots[RSC_TX_SLOTS];
  uint8_t           head;
  uint8_t           count;              // queued messages, including those in flight
  uint8_t           inFlight;
  uint8_t           base;
  uint8_t           rtoCount;
  bool              timerOn;
  bool              resend;
  bool              peerRnr;
  uint32_t          rtoStart;
  uint32_t          rto;
  uint32_t          lastTx;
  bool              txEver;

  // receive
  uint8_t           rxNext;
  bool              ackDue;
  bool              rnr;                // this side refused a message: "receiver not ready"
  bool              resumeReq;          // rscResume() was called
  rscCobsDecoder_t  dec;
  uint8_t           rx[RSC_RX_MAX];

  // requests
  rscPending_t      pending[RSC_MAX_REQUESTS];
  uint8_t           nextTag;

#if RSC_STATS
  rscStats_t        stats;
#endif
};


// API =========================================================================

// Fill cfg with the build defaults (window RSC_WINDOW, mtu RSC_MTU, retries RSC_RETRIES)
void        rscConfigDefaults( rscConfig_t* cfg );

// Set up a link. ops->write and ops->millis are required. The link starts to sync at the first
// rscService(). Returns RSC_OK, RSC_ERROR_NULL or RSC_ERROR_BAD_ARG.
rscError_t  rscInit( rscLink_t* link, const rscConfig_t* cfg, const rscOps_t* ops );

// Pass received bytes to the link. Complete frames are handled at once, and their callbacks run
// in this call. Call rscService() afterwards, so that acknowledgements leave promptly.
void        rscFeed( rscLink_t* link, const uint8_t* data, size_t length );

// Run timers and transmission. Returns the ms until it needs to run again (0: at once).
// rscFeed() and rscService() of one link must run in the same thread.
uint32_t    rscService( rscLink_t* link );

// Queue an event (no response expected). Delivered once and in order while the link holds.
rscError_t  rscSend( rscLink_t* link, uint8_t type, const uint8_t* data, uint16_t length );

// Queue a request. callback runs for each PROGRESS and once for the final outcome. timeoutMs
// counts from now, and restarts at each PROGRESS. *tag (optional) receives the request tag.
rscError_t  rscRequest( rscLink_t* link, uint8_t type, const uint8_t* data, uint16_t length,
                        uint32_t timeoutMs, rscResponseCb_t callback, void* user, uint8_t* tag );

// Answer the request tag: with data and an application type, ACK, NACK, or PROGRESS
rscError_t  rscRespond( rscLink_t* link, uint8_t tag, uint8_t type, const uint8_t* data, uint16_t length );
rscError_t  rscAck( rscLink_t* link, uint8_t tag, const uint8_t* data, uint16_t length );
rscError_t  rscNack( rscLink_t* link, uint8_t tag, int8_t error, const uint8_t* data, uint16_t length );
rscError_t  rscProgress( rscLink_t* link, uint8_t tag, const uint8_t* data, uint16_t length );

// After the message callback refused a message: accept messages again
void        rscResume( rscLink_t* link );

bool        rscIsUp( const rscLink_t* link );
// Data length the link carries now: the smaller MTU of the two sides (the local one before sync)
uint16_t    rscMtu( const rscLink_t* link );
// Messages queued and not yet acknowledged
uint8_t     rscQueued( const rscLink_t* link );
#if RSC_STATS
const rscStats_t* rscGetStats( const rscLink_t* link );
#endif

#ifdef __cplusplus
}
#endif
