/* wmtr host executor: runs remoted winemetal calls against Metal (DXMT's own winemetal_unix.c).
 * Embedded either in a standalone server process (two-process prototype) or in the VMM (libkrun
 * virtio-gpu context). The embedder owns shared memory and wake-ups; see struct wmtr_host_ops. */
#ifndef WMTR_HOST_H
#define WMTR_HOST_H
#include <stdint.h>

struct wmtr_host;

struct wmtr_host_ops {
  /* host address and size of arena blob `blob` (registered by the embedder), NULL if unknown */
  void *(*blob_ptr)(void *ctx, uint32_t blob, uint64_t *size);
  /* wake a client thread waiting on reply slot `slot` (or WMTR_EVENT_SLOT) */
  void (*wake)(void *ctx, uint32_t slot);
  /* presentation: create a view + CAMetalLayer for a client window; returns the view handle and
   * stores the CAMetalLayer* in *layer. NULL ops -> the call fails. */
  uint64_t (*create_view)(void *ctx, uint64_t hwnd, void *mtl_device, uint64_t *layer);
  void (*release_view)(void *ctx, uint64_t view);
  /* map a client path (shader caches, binary archives) to a host path; NULL op keeps it */
  const char *(*map_path)(void *ctx, const char *client_path, char *buf, uint64_t buflen);
};

/* `ring` is the host mapping of blob 0 (struct wmtr_ring + data + slots), already initialised by
 * the client. Starts the executor thread. */
struct wmtr_host *wmtr_host_create(void *ring, const struct wmtr_host_ops *ops, void *ctx);
/* the client kicked: the executor may be idle */
void wmtr_host_kick(struct wmtr_host *h);
void wmtr_host_destroy(struct wmtr_host *h);
/* done_seq of reply slot `slot`, or event_head for WMTR_EVENT_SLOT */
uint64_t wmtr_host_progress(struct wmtr_host *h, uint32_t slot);
/* default presenter (a window per HWND on the embedder's main thread); usable as create_view/release_view */
uint64_t wmtr_present_create_view(void *ctx, uint64_t hwnd, void *mtl_device, uint64_t *layer);
void wmtr_present_release_view(void *ctx, uint64_t view);
/* counters for measurements */
struct wmtr_host_stats { uint64_t requests, sync_requests, bytes, worker_calls, idle_sleeps; };
void wmtr_host_get_stats(struct wmtr_host *h, struct wmtr_host_stats *out);
/* Names of shared resources (DXMT's D3D11 MISC_SHARED textures and shared fences, calls 120-124) that live clients
 * have registered. The Mach ports stay in this process: every client's executor runs here. */
uint32_t wmtr_host_shared_names(void);
/* How long the executor polls an empty ring before it sleeps (default WMTR_HOST_SPIN_US_DEFAULT us, env
 * WMTR_HOST_SPIN_US at create). Longer: fewer kicks (guest VM exits) for host CPU. VRChat (DXMT): 2 us -> 100 us
 * cut kicks from 290 to 64 a frame and executor sleeps from 97 to 15, idle fps 82 -> 99, host CPU of the VM process
 * unchanged (347% vs 336%), n=1 each. */
#define WMTR_HOST_SPIN_US_DEFAULT 100
void wmtr_host_set_spin(struct wmtr_host *h, uint64_t ns);

#endif
