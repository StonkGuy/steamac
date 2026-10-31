/* wmtr: remoting of DXMT's winemetal unix calls from a client (the Windows process side, inside a
 * VM guest or a second macOS process) to a host executor that runs the real Metal implementation.
 *
 * Transport-independent wire format. Shared memory ("arena") is made of blobs; the client and the
 * host each map every blob, so data is referenced as (blob, offset), never as a pointer.
 *
 *   ring blob (blob 0):  struct wmtr_ring header, then the request ring, then reply slots
 *   arena blobs (1..):   page-granular allocations for buffer memory and large transfers
 *
 * A request is one call: header, the call's params struct (copied verbatim; pointer fields are
 * rewritten to WMTR_REF values), then "extra" bytes the pointers referred to. A sync request gets
 * its reply in the calling thread's reply slot: the params struct as the host left it plus output
 * bytes. Async requests have no reply. Both sides use the same struct layouts (winemetal_thunks.h
 * is pack(8) with fixed-width fields; client and host are LP64 little-endian). */
#ifndef WMTR_PROTO_H
#define WMTR_PROTO_H
#include <stdint.h>

#define WMTR_VERSION 2 /* 2: client-assigned handles (WMTR_F_CLIENT_HANDLE) */
#define WMTR_PAGE 16384u                 /* host page size on Apple silicon */
#define WMTR_RING_BYTES (8u << 20)       /* request ring */
#define WMTR_SLOTS 64                    /* reply slots (one per client thread) */
#define WMTR_SLOT_BYTES (64u << 10)      /* reply slot payload */
#define WMTR_ARENA_BLOB_BYTES (64u << 20)/* arena blob size */
#define WMTR_MAX_BLOBS 1024
/* winemetal unix calls: DXMT's table (src/wmtr_calls.inc, 0..145) plus WMTVRSubmit (146) */
#define WMTR_NCALLS 147

/* a pointer field in params/extra rewritten on the wire */
#define WMTR_REF_EXTRA 0x5752000000000000ull /* | offset into this request's extra bytes */
#define WMTR_REF_ARENA 0x5741000000000000ull /* | blob << 32 | offset (arena memory) */
#define WMTR_REF_HOST 0x5748000000000000ull  /* unused tag reserved: value is already a host pointer */
#define WMTR_REF_MASK 0xffff000000000000ull

struct wmtr_req {
  uint32_t size;        /* total bytes of this record, 8-aligned, including the header */
  uint16_t call;        /* winemetal unix call index (DXMT 4ddb20e numbering) */
  uint16_t flags;       /* WMTR_F_* */
  uint32_t slot;        /* reply slot (sync calls) */
  uint32_t params_size; /* bytes of the params struct that follow */
  uint64_t seq;         /* per-slot sequence number of a sync call */
  /* params (params_size, padded to 8) then extra bytes */
};
#define WMTR_F_SYNC 1u    /* caller waits for a reply */
#define WMTR_F_WRAP 2u    /* padding record: skip to ring start */
#define WMTR_F_CONTROL 4u /* transport control (call = WMTR_CTL_*) */
#define WMTR_F_CLIENT_HANDLE 8u /* the call's `ret` holds a handle the client assigned (see below) */

/* Client-assigned handles: creations whose result the caller only passes back to later calls (autorelease
 * pools, command encoders) are sent async with `ret` set to a handle the client chose. The host records
 * handle -> object; a later call whose first params word (the receiver: retain/release, endEncoding,
 * encodeCommands, setLabel) is such a handle gets the object. Ring order puts every use after its creation.
 * A creation that failed is reported by the host's log, and the calls that use its handle fail there. */
#define WMTR_HANDLE_TAG 0x5743000000000000ull  /* "WC"; never a host pointer (bits 63..48) */
#define WMTR_HANDLE_POOL 0x0000800000000000ull /* | slot << 32 | n: autorelease pool of reply slot `slot` */
#define WMTR_HANDLE_SLOTS 65536                /* encoder handles live in host table entry (handle & mask) */

enum {
  WMTR_CTL_HELLO = 1,    /* params: struct wmtr_hello */
  WMTR_CTL_BLOB_ADD = 2, /* params: struct wmtr_blob_add; host maps a new arena blob */
  WMTR_CTL_PING = 3,     /* sync no-op (latency measurement) */
  WMTR_CTL_VR_QUERY = 4, /* sync: one guest VR message (guest/vr/include/steamac_vr_guest.h) in the params, the
                            executor's VR exporter overwrites it with the reply; params_size >= both */
};
struct wmtr_hello { uint32_t version, slots; uint64_t ring_bytes, slot_bytes; };
struct wmtr_blob_add { uint32_t blob; uint32_t reserved; uint64_t size; };

struct wmtr_slot {
  volatile uint64_t done_seq; /* host stores seq when the reply is complete (release) */
  volatile uint32_t status;   /* NTSTATUS of the call */
  volatile uint32_t out_size; /* params + output bytes written to data */
  volatile uint32_t waiting;  /* client sleeps on the transport's wait primitive */
  uint32_t pad[11];
  uint8_t data[WMTR_SLOT_BYTES];
};

struct wmtr_free_entry { uint32_t blob, offset, size, pad; };
#define WMTR_FREE_RING 4096
/* listener 0 never occurs; event_handle 0 means "listener destroyed: stop" */
struct wmtr_event { uint64_t listener, event_handle; };
#define WMTR_EVENT_RING 4096
#define WMTR_EVENT_SLOT WMTR_SLOTS /* pseudo slot id used to wait for events */

struct wmtr_ring {
  uint32_t magic, version;
  volatile uint64_t head;      /* client: bytes produced (monotonic) */
  uint8_t pad0[48];
  volatile uint64_t tail;      /* host: bytes consumed (monotonic) */
  volatile uint32_t host_idle; /* host executor is asleep; the client must kick */
  uint8_t pad1[44];
  /* host -> client: arena ranges whose MTLBuffer was deallocated (NoCopy deallocator) */
  volatile uint64_t free_head; /* host produced */
  volatile uint64_t free_tail; /* client consumed */
  uint8_t pad2[48];
  struct wmtr_free_entry free_ring[WMTR_FREE_RING];
  /* host -> client: Win32 events to set (MTLSharedEvent notifications); wait slot WMTR_EVENT_SLOT */
  volatile uint64_t event_head; /* host produced; the client waits on it */
  volatile uint64_t event_tail; /* client consumed */
  uint8_t pad3[48];
  struct wmtr_event event_ring[WMTR_EVENT_RING];
};

#define WMTR_RING_MAGIC 0x52544d57u /* "WMTR" */
#define WMTR_RING_DATA_OFFSET ((sizeof(struct wmtr_ring) + 4095) & ~4095ull)
#define WMTR_SLOTS_OFFSET (WMTR_RING_DATA_OFFSET + WMTR_RING_BYTES)
#define WMTR_RING_BLOB_BYTES                                                                                      \
  ((WMTR_SLOTS_OFFSET + (uint64_t)WMTR_SLOTS * sizeof(struct wmtr_slot) + WMTR_PAGE - 1) & ~(uint64_t)(WMTR_PAGE - 1))

#endif
