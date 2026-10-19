// SPDX-License-Identifier: GPL-3.0-or-later
// macOS shims for WiVRn's common/wivrn_sockets.cpp, force-included (-include) so the vendored upstream files stay
// unmodified (msghdr::msg_iovlen is an int on macOS: the target also builds with -Wno-c++11-narrowing). macOS has no recvmmsg/sendmmsg/MSG_NOSIGNAL; steamac-vrhost ignores SIGPIPE process-wide instead.
#pragma once
#ifdef __APPLE__
#include <cerrno>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <netinet/in.h>

#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct mmsghdr
{
	struct msghdr msg_hdr;
	unsigned int msg_len;
};

// Same contract as Linux for the flags WiVRn uses: returns the number of messages received, stops at the first
// would-block once at least one message was read.
static inline int recvmmsg(int fd, struct mmsghdr * msgvec, unsigned int vlen, int flags, struct timespec *)
{
	unsigned int i = 0;
	for (; i < vlen; ++i)
	{
		ssize_t r = ::recvmsg(fd, &msgvec[i].msg_hdr, flags | (i ? MSG_DONTWAIT : 0));
		if (r < 0)
		{
			if (i > 0 and (errno == EAGAIN or errno == EWOULDBLOCK))
				break;
			return i > 0 ? int(i) : -1;
		}
		msgvec[i].msg_len = unsigned(r);
	}
	return int(i);
}

static inline int sendmmsg(int fd, struct mmsghdr * msgvec, unsigned int vlen, int flags)
{
	unsigned int i = 0;
	for (; i < vlen; ++i)
	{
		ssize_t r = ::sendmsg(fd, &msgvec[i].msg_hdr, flags);
		if (r < 0)
			return i > 0 ? int(i) : -1;
		msgvec[i].msg_len = unsigned(r);
	}
	return int(i);
}
#endif
