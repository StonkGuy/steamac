/* control messages of the two-process socket transport */
#ifndef WMTR_SOCK_H
#define WMTR_SOCK_H
#include <stdint.h>
enum { WMTR_SOCK_RING = 1, WMTR_SOCK_BLOB = 2, WMTR_SOCK_KICK = 3, WMTR_SOCK_BLOB_DEL = 4 /* no fd, no ack */ };
struct wmtr_sock_msg { uint32_t type, blob; uint64_t size; }; /* RING/BLOB carry an fd (SCM_RIGHTS) */
#endif
