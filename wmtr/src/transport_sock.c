/* wmtr client transport for two processes on one Mac: POSIX shm blobs whose fds are passed to the
 * server over a unix socket ($WMTR_SOCKET, default /tmp/wmtr.sock). Kick = one control message.
 * Waiting = spin, then yield, then short sleeps (the server does not wake clients). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "wmtr_proto.h"
#include "wmtr_spin.h"
#include "wmtr_transport.h"
#include "wmtr_sock.h"

struct sock_priv { int fd; };

static int
send_msg(int s, const struct wmtr_sock_msg *m, int fd) {
  struct iovec iov = {(void *)m, sizeof *m};
  char cbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr mh = {0};
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  if (fd >= 0) {
    memset(cbuf, 0, sizeof cbuf);
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
  }
  for (;;) {
    ssize_t n = sendmsg(s, &mh, 0);
    if (n == (ssize_t)sizeof *m) return 0;
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
}

static void *
make_shm(uint64_t size, int *fd_out) {
  char name[64];
  static int seq;
  snprintf(name, sizeof name, "/wmtr.%d.%d", (int)getpid(), seq++);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return NULL;
  shm_unlink(name);
  if (ftruncate(fd, (off_t)size) < 0) { close(fd); return NULL; }
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); return NULL; }
  *fd_out = fd;
  return p;
}

static void *
s_open_ring(struct wmtr_transport *t, uint64_t size) {
  struct sock_priv *sp = t->priv;
  const char *path = getenv("WMTR_SOCKET");
  if (!path) path = "/tmp/wmtr.sock";
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {0};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof a.sun_path - 1);
  if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { fprintf(stderr, "wmtr: connect %s: %s\n", path, strerror(errno)); return NULL; }
  sp->fd = s;
  int fd;
  struct wmtr_ring *R = make_shm(size, &fd);
  if (!R) return NULL;
  memset(R, 0, sizeof *R);
  R->magic = WMTR_RING_MAGIC; R->version = WMTR_VERSION;
  struct wmtr_sock_msg m = {WMTR_SOCK_RING, 0, size};
  if (send_msg(s, &m, fd) < 0) return NULL;
  close(fd);
  return R;
}

static void *
s_add_blob(struct wmtr_transport *t, uint32_t blob, uint64_t size) {
  struct sock_priv *sp = t->priv;
  int fd;
  void *p = make_shm(size, &fd);
  if (!p) return NULL;
  struct wmtr_sock_msg m = {WMTR_SOCK_BLOB, blob, size};
  int rc = send_msg(sp->fd, &m, fd);
  close(fd);
  if (rc < 0) return NULL;
  /* the server maps it when it reads the message; requests using the blob come later on the ring,
   * which the server only reads after draining the socket (see server) */
  char ack;
  if (read(sp->fd, &ack, 1) != 1) return NULL;
  return p;
}

static void
s_release_blob(struct wmtr_transport *t, uint32_t blob, void *base, uint64_t size) {
  struct sock_priv *sp = t->priv;
  munmap(base, size);
  struct wmtr_sock_msg m = {WMTR_SOCK_BLOB_DEL, blob, size};
  send_msg(sp->fd, &m, -1);
}

static void
s_kick(struct wmtr_transport *t) {
  struct sock_priv *sp = t->priv;
  struct wmtr_sock_msg m = {WMTR_SOCK_KICK, 0, 0};
  send_msg(sp->fd, &m, -1);
}

static void
s_wait(struct wmtr_transport *t, volatile uint64_t *done, uint64_t seq, uint32_t slot, uint64_t spin_ns) {
  (void)t; (void)slot;
  if (wmtr_spin_wait(done, seq, spin_ns)) return;
  struct timespec ts = {0, 50000};
  while (__atomic_load_n(done, __ATOMIC_ACQUIRE) < seq) nanosleep(&ts, NULL);
}

struct wmtr_transport *
wmtr_transport_create(void) {
  struct wmtr_transport *t = calloc(1, sizeof *t);
  struct sock_priv *sp = calloc(1, sizeof *sp);
  sp->fd = -1;
  t->priv = sp;
  t->open_ring = s_open_ring; t->add_blob = s_add_blob; t->kick = s_kick; t->wait = s_wait;
  t->release_blob = s_release_blob;
  return t;
}
