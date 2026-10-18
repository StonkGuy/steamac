/* wmtr client transport: how the client gets shared memory and wakes/waits for the host.
 * Implementations: socket+shm (two macOS processes, P2 prototype) and virtio-gpu (VM guest). */
#ifndef WMTR_TRANSPORT_H
#define WMTR_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>

struct wmtr_transport {
  /* create the ring blob (blob 0) and connect; returns its client mapping */
  void *(*open_ring)(struct wmtr_transport *t, uint64_t size);
  /* create arena blob number `blob` of `size` bytes, make the host map it; returns client mapping */
  void *(*add_blob)(struct wmtr_transport *t, uint32_t blob, uint64_t size);
  /* wake the host executor (it set host_idle) */
  void (*kick)(struct wmtr_transport *t);
  /* block until *done_seq >= seq (the host kicks waiting slots); poll for at most spin_ns first
   * (wmtr_spin.h: 0 for calls the host answers in milliseconds) */
  void (*wait)(struct wmtr_transport *t, volatile uint64_t *done_seq, uint64_t seq, uint32_t slot,
               uint64_t spin_ns);
  /* optional: give arena blob `blob` (client mapping `base`, `size` bytes, no live range left and the
   * host holds no buffer in it) back to the host; afterwards the number may be added again.
   * NULL: blobs are kept for the life of the process */
  void (*release_blob)(struct wmtr_transport *t, uint32_t blob, void *base, uint64_t size);
  void *priv;
};

struct wmtr_transport *wmtr_transport_create(void);

#endif
