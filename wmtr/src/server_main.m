/* wmtr-server: standalone host executor for the two-process prototype (P2). Listens on
 * $WMTR_SOCKET (default /tmp/wmtr.sock); each connection is one client process (one ring).
 * Presentation: one NSWindow with a CAMetalLayer per client HWND. Runs natively on arm64. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wmtr_host.h"
#include "wmtr_proto.h"
#include "wmtr_sock.h"

struct conn {
  int fd;
  void *blob_base[WMTR_MAX_BLOBS];
  uint64_t blob_size[WMTR_MAX_BLOBS];
  struct wmtr_host *host;
};

static void *
c_blob_ptr(void *ctx, uint32_t blob, uint64_t *size) {
  struct conn *c = ctx;
  if (blob >= WMTR_MAX_BLOBS) return NULL;
  *size = c->blob_size[blob];
  return c->blob_base[blob];
}

static void
c_wake(void *ctx, uint32_t slot) { (void)ctx; (void)slot; /* clients poll */ }

static const struct wmtr_host_ops ops = {c_blob_ptr, c_wake, wmtr_present_create_view, wmtr_present_release_view, NULL};

static int
recv_msg(int s, struct wmtr_sock_msg *m, int *fd) {
  struct iovec iov = {m, sizeof *m};
  char cbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr mh = {0};
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
  ssize_t n = recvmsg(s, &mh, MSG_WAITALL);
  if (n != (ssize_t)sizeof *m) return -1;
  *fd = -1;
  struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
  if (c && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof *fd);
  return 0;
}

static void *
conn_thread(void *arg) {
  struct conn *c = arg;
  struct wmtr_sock_msg m;
  int fd;
  while (recv_msg(c->fd, &m, &fd) == 0) {
    if (m.type == WMTR_SOCK_RING || m.type == WMTR_SOCK_BLOB) {
      uint32_t b = m.type == WMTR_SOCK_RING ? 0 : m.blob;
      void *p = mmap(NULL, m.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      close(fd);
      if (p == MAP_FAILED || b >= WMTR_MAX_BLOBS) { fprintf(stderr, "wmtr-server: map blob %u failed\n", b); break; }
      c->blob_base[b] = p; c->blob_size[b] = m.size;
      if (b == 0) {
        c->host = wmtr_host_create(p, &ops, c);
        if (!c->host) break;
        fprintf(stderr, "wmtr-server: client connected, ring %llu bytes\n", (unsigned long long)m.size);
      } else {
        char ack = 1;
        write(c->fd, &ack, 1);
      }
    } else if (m.type == WMTR_SOCK_BLOB_DEL && m.blob && m.blob < WMTR_MAX_BLOBS && c->blob_base[m.blob]) {
      /* the client released an idle arena blob: no live range or buffer is left in it */
      munmap(c->blob_base[m.blob], c->blob_size[m.blob]);
      c->blob_base[m.blob] = NULL; c->blob_size[m.blob] = 0;
    } else if (m.type == WMTR_SOCK_KICK && c->host) {
      wmtr_host_kick(c->host);
    }
  }
  if (c->host) {
    struct wmtr_host_stats st;
    wmtr_host_get_stats(c->host, &st);
    fprintf(stderr, "wmtr-server: client gone: requests=%llu sync=%llu bytes=%llu worker=%llu sleeps=%llu\n",
            (unsigned long long)st.requests, (unsigned long long)st.sync_requests, (unsigned long long)st.bytes,
            (unsigned long long)st.worker_calls, (unsigned long long)st.idle_sleeps);
    wmtr_host_destroy(c->host);
  }
  close(c->fd);
  return NULL;
}

static void *
listen_thread(void *arg) {
  const char *path = arg;
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {0};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof a.sun_path - 1);
  unlink(path);
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) {
    fprintf(stderr, "wmtr-server: %s: %s\n", path, strerror(errno));
    exit(1);
  }
  fprintf(stderr, "wmtr-server: listening on %s\n", path);
  for (;;) {
    int fd = accept(s, NULL, NULL);
    if (fd < 0) continue;
    struct conn *c = calloc(1, sizeof *c);
    c->fd = fd;
    pthread_t t;
    pthread_create(&t, NULL, conn_thread, c);
    pthread_detach(t);
  }
}

int
main(int argc, char **argv) {
  @autoreleasepool {
    const char *path = getenv("WMTR_SOCKET");
    if (!path) path = "/tmp/wmtr.sock";
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    pthread_t t;
    pthread_create(&t, NULL, listen_thread, (void *)path);
    [NSApp run];
  }
  return 0;
}
