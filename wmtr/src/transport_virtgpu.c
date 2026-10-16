/* wmtr client transport inside a VM guest: a virtio-gpu context of capset WMTR_CAPSET_ID on
 * /dev/dri/renderD128.
 *   blobs: VIRTGPU_BLOB_MEM_HOST3D, mappable; blob_id = wmtr blob number; the host allocates them
 *          (shared memory it also wraps with MTLBuffers) and the guest maps them with DRM_VIRTGPU_MAP
 *   kick:  EXECBUFFER of a struct wmtr_vgpu_cmd {KICK}
 *   wait:  poll for the caller's budget (wmtr_spin.h), then EXECBUFFER {WAIT slot seq} on ring 1 + slot % 62 (63 for events) with an
 *          out-fence the host signals once *done >= seq; poll() the fence fd
 * The UAPI structs are copied from include/uapi/drm/virtgpu_drm.h (stable kernel ABI). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "wmtr_proto.h"
#include "wmtr_spin.h"
#include "wmtr_transport.h"
#include "wmtr_vgpu.h"

#define DRM_IOCTL_BASE 'd'
#define DRM_COMMAND_BASE 0x40
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)

struct drm_gem_close { uint32_t handle, pad; };
struct drm_virtgpu_map { uint64_t offset; uint32_t handle, pad; };
struct drm_virtgpu_execbuffer {
  uint32_t flags, size;
  uint64_t command, bo_handles;
  uint32_t num_bo_handles;
  int32_t fence_fd;
  uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
  uint64_t in_syncobjs, out_syncobjs;
};
struct drm_virtgpu_getparam { uint64_t param, value; };
struct drm_virtgpu_resource_create_blob {
  uint32_t blob_mem, blob_flags, bo_handle, res_handle;
  uint64_t size;
  uint32_t pad, cmd_size;
  uint64_t cmd, blob_id;
};
struct drm_virtgpu_context_set_param { uint64_t param, value; };
struct drm_virtgpu_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };

#define DRM_IOCTL_GEM_CLOSE DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_VIRTGPU_MAP DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_GETPARAM DRM_IOWR(DRM_COMMAND_BASE + 0x03, struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB DRM_IOWR(DRM_COMMAND_BASE + 0x0a, struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT DRM_IOWR(DRM_COMMAND_BASE + 0x0b, struct drm_virtgpu_context_init)

#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIRTGPU_EXECBUF_RING_IDX 0x04
#define VIRTGPU_BLOB_MEM_HOST3D 0x0002
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS 0x0002
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7

struct vg_priv { int fd; uint32_t ring_bo; uint32_t blob_bo[WMTR_MAX_BLOBS]; };

/* WAIT commands submitted (tests: tests/wmtr_wait_test.c) */
uint64_t wmtr_vgpu_wait_submits;

static int
xioctl(int fd, unsigned long req, void *arg) {
  int r;
  do r = ioctl(fd, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
  return r;
}

static int
submit(struct vg_priv *p, const struct wmtr_vgpu_cmd *c, uint32_t ring, int *fence_out) {
  struct drm_virtgpu_execbuffer eb = {0};
  eb.command = (uint64_t)(uintptr_t)c;
  eb.size = sizeof *c;
  eb.fence_fd = -1;
  if (fence_out) { eb.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX; eb.ring_idx = ring; }
  if (xioctl(p->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb) < 0) return -errno;
  if (fence_out) *fence_out = eb.fence_fd;
  return 0;
}

static void *
map_blob(struct vg_priv *p, uint32_t blob, uint64_t size, uint32_t *bo_out) {
  struct drm_virtgpu_resource_create_blob cb = {0};
  cb.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
  cb.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE | VIRTGPU_BLOB_FLAG_USE_SHAREABLE;
  cb.size = size;
  cb.blob_id = blob;
  if (xioctl(p->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &cb) < 0) {
    fprintf(stderr, "wmtr: create blob %u (%llu bytes): %s\n", blob, (unsigned long long)size, strerror(errno));
    return NULL;
  }
  struct drm_virtgpu_map m = {0};
  m.handle = cb.bo_handle;
  if (xioctl(p->fd, DRM_IOCTL_VIRTGPU_MAP, &m) < 0) { fprintf(stderr, "wmtr: map blob %u: %s\n", blob, strerror(errno)); return NULL; }
  void *v = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, (off_t)m.offset);
  if (v == MAP_FAILED) { fprintf(stderr, "wmtr: mmap blob %u: %s\n", blob, strerror(errno)); return NULL; }
  if (bo_out) *bo_out = cb.bo_handle;
  return v;
}

static void *
v_open_ring(struct wmtr_transport *t, uint64_t size) {
  struct vg_priv *p = t->priv;
  const char *dev = getenv("WMTR_DRM");
  p->fd = open(dev ? dev : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (p->fd < 0) { fprintf(stderr, "wmtr: open render node: %s\n", strerror(errno)); return NULL; }
  struct drm_virtgpu_getparam gp = {VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, 0};
  uint64_t mask = 0;
  gp.value = (uint64_t)(uintptr_t)&mask;
  if (xioctl(p->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) < 0 || !(mask & (1ull << WMTR_CAPSET_ID))) {
    fprintf(stderr, "wmtr: host offers no wmtr capset (mask %llx)\n", (unsigned long long)mask);
    return NULL;
  }
  struct drm_virtgpu_context_set_param ps[2] = {{VIRTGPU_CONTEXT_PARAM_CAPSET_ID, WMTR_CAPSET_ID},
                                                {VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 64}};
  struct drm_virtgpu_context_init ci = {2, 0, (uint64_t)(uintptr_t)ps};
  if (xioctl(p->fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci) < 0) { fprintf(stderr, "wmtr: context init: %s\n", strerror(errno)); return NULL; }
  struct wmtr_ring *R = map_blob(p, 0, size, &p->ring_bo);
  if (!R) return NULL;
  memset(R, 0, sizeof *R);
  R->magic = WMTR_RING_MAGIC; R->version = WMTR_VERSION;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  struct wmtr_vgpu_cmd c = {WMTR_VGPU_RING, 0, 0, 0};
  if (submit(p, &c, 0, NULL) < 0) return NULL;
  return R;
}

static void *
v_add_blob(struct wmtr_transport *t, uint32_t blob, uint64_t size) {
  struct vg_priv *p = t->priv;
  return blob < WMTR_MAX_BLOBS ? map_blob(p, blob, size, &p->blob_bo[blob]) : NULL;
}

/* unmapping and closing the last handle makes the kernel detach the resource from the context,
 * unmap it from the guest (the host drops its stage-2 mapping) and unref it (the host frees it) */
static void
v_release_blob(struct wmtr_transport *t, uint32_t blob, void *base, uint64_t size) {
  struct vg_priv *p = t->priv;
  munmap(base, size);
  struct drm_gem_close gc = {p->blob_bo[blob], 0};
  if (xioctl(p->fd, DRM_IOCTL_GEM_CLOSE, &gc) < 0) fprintf(stderr, "wmtr: close blob %u: %s\n", blob, strerror(errno));
  p->blob_bo[blob] = 0;
}

static void
v_kick(struct wmtr_transport *t) {
  struct wmtr_vgpu_cmd c = {WMTR_VGPU_KICK, 0, 0, 0};
  submit(t->priv, &c, 0, NULL);
}

static void
v_wait(struct wmtr_transport *t, volatile uint64_t *done, uint64_t seq, uint32_t slot, uint64_t spin_ns) {
  if (wmtr_spin_wait(done, seq, spin_ns)) return;
  struct vg_priv *p = t->priv;
  uint32_t ring = slot == WMTR_EVENT_SLOT ? 63 : 1 + slot % 62;
  /* At most one WAIT in flight per waiting thread: the host answers a fenced command only when its
   * fence completes, so every pending WAIT holds control-queue descriptors. Re-submitting on each
   * poll timeout (an idle event listener waits indefinitely) filled the queue and stalled every
   * virtio-gpu client in the guest, Venus and the compositor included. A new WAIT is only sent once
   * the previous fence has signalled (fences on a shared ring can signal early). */
  int fence = -1;
  while (__atomic_load_n(done, __ATOMIC_ACQUIRE) < seq) {
    if (fence < 0) {
      struct wmtr_vgpu_cmd c = {WMTR_VGPU_WAIT, slot, seq, 0};
      if (submit(p, &c, ring, &fence) < 0 || fence < 0) { fence = -1; usleep(200); continue; }
      __atomic_fetch_add(&wmtr_vgpu_wait_submits, 1, __ATOMIC_RELAXED);
    }
    struct pollfd pfd = {fence, POLLIN, 0};
    if (poll(&pfd, 1, 100) > 0) { close(fence); fence = -1; }
  }
  if (fence >= 0) close(fence);
}

struct wmtr_transport *
wmtr_transport_create(void) {
  struct wmtr_transport *t = calloc(1, sizeof *t);
  struct vg_priv *p = calloc(1, sizeof *p);
  p->fd = -1;
  t->priv = p;
  t->open_ring = v_open_ring; t->add_blob = v_add_blob; t->kick = v_kick; t->wait = v_wait;
  t->release_blob = v_release_blob;
  return t;
}
