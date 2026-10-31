// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include "vrlink_server.h"
#include "vrlink.h"
#include "vrlink_validate.h"

#include <IOSurface/IOSurface.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mach/mach.h>
#include <poll.h>
#include <servers/bootstrap.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>

extern "C" kern_return_t bootstrap_register2(mach_port_t, name_t, mach_port_t, uint64_t);

namespace vrhost
{
namespace
{
int64_t now_ns()
{
	return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

struct Swapchain
{
	vrl_swapchain desc{};
	IOSurfaceRef surface[VRL_MAX_IMAGES]{};
	~Swapchain()
	{
		for (auto s: surface)
			if (s)
				CFRelease(s);
	}
};
} // namespace

struct VrLinkServer::impl
{
	int fd;
	mach_port_t port = MACH_PORT_NULL;
	Sink sink;
	MessageHandler other;
	SinkError on_sink_error;
	std::thread thread;
	std::atomic<bool> quit{false}, closed{false};
	std::atomic<uint64_t> frames{0};
	std::unordered_map<uint32_t, std::unique_ptr<Swapchain>> chains;

	bool receive_ports(uint32_t id, Swapchain & sc)
	{
		// the vm side sends the Mach message before the VRL_SWAPCHAIN packet; ports for other (older) ids
		// that are still queued are dropped
		for (int tries = 0; tries < 8; tries++)
		{
			vrl_mach_swapchain_rcv m{};
			kern_return_t kr = mach_msg(&m.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, port, 2000,
			                            MACH_PORT_NULL);
			if (kr != KERN_SUCCESS)
				return false;
			bool match = m.msg.header.msgh_id == VRL_MACH_SWAPCHAIN_ID && m.msg.swapchain_id == id;
			for (uint32_t i = 0; i < VRL_MAX_IMAGES; i++)
			{
				mach_port_t sp = m.msg.ports[i].name;
				if (sp == MACH_PORT_NULL)
					continue;
				if (match && i < m.msg.image_count && i < sc.desc.image_count)
				{
					IOSurfaceRef s = IOSurfaceLookupFromMachPort(sp);
					if (s && !vrl_surface_ok(sc.desc, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), IOSurfaceGetBytesPerElement(s)))
					{
						fprintf(stderr, "vrhost: swapchain %u image %u does not match its description\n", id, i);
						CFRelease(s);
						s = nullptr;
					}
					sc.surface[i] = s;
				}
				mach_port_deallocate(mach_task_self(), sp);
			}
			if (match)
				return true;
		}
		return false;
	}

	void send_release(uint32_t chain, uint32_t image, uint64_t frame)
	{
		vrl_release r{{VRL_RELEASE, sizeof r}, chain, image, frame, now_ns()};
		::send(fd, &r, sizeof r, 0);
	}

	void run()
	{
		union
		{
			vrl_header h;
			vrl_hello hello;
			vrl_swapchain sc;
			vrl_submit sub;
			uint8_t raw[4096];
		} u;
		while (!quit)
		{
			pollfd pf{fd, POLLIN, 0};
			if (poll(&pf, 1, 100) <= 0)
			{
				// a datagram socketpair gives no end-of-file when the vm side closes (macOS): the peer is gone
				// when the socket is no longer connected
				sockaddr_storage peer;
				socklen_t len = sizeof peer;
				if (getpeername(fd, (sockaddr *)&peer, &len) != 0 && errno == ENOTCONN)
				{
					closed = true;
					return;
				}
				continue;
			}
			ssize_t n = recv(fd, &u, sizeof u, 0);
			if (n <= 0)
			{
				closed = true;
				return;
			}
			if (n < ssize_t(sizeof u.h) || u.h.size != uint32_t(n))
				continue;
			switch (u.h.type)
			{
				case VRL_HELLO:
					if (n != sizeof u.hello or u.hello.version != VRL_VERSION)
					{
						fprintf(stderr, "vrhost: vrlink version %u, expected %u\n", u.hello.version, VRL_VERSION);
						break;
					}
					if (other)
						other(&u, size_t(n));
					break;
				case VRL_SWAPCHAIN: {
					if (n != sizeof u.sc || !vrl_swapchain_ok(u.sc))
					{
						fprintf(stderr, "vrhost: invalid swapchain description dropped\n");
						break;
					}
					auto sc = std::make_unique<Swapchain>();
					sc->desc = u.sc;
					if (!receive_ports(u.sc.id, *sc))
					{
						fprintf(stderr, "vrhost: swapchain %u: no ports\n", u.sc.id);
						break;
					}
					chains[u.sc.id] = std::move(sc);
					break;
				}
				case VRL_SUBMIT: {
					if (n != sizeof u.sub)
						break;
					LinkFrame f{};
					f.frame_id = u.sub.frame_id;
					f.display_time = u.sub.display_time;
					f.app_submit_time = u.sub.app_submit_time;
					f.ready_time = u.sub.ready_time;
					f.received_time = now_ns();
					memcpy(f.pose, u.sub.pose, sizeof f.pose);
					bool ok = vrl_poses_ok(u.sub);
					bool fov_ok = vrl_fov_ok(u.sub.fov[0]) && vrl_fov_ok(u.sub.fov[1]);
					for (int e = 0; e < 2; e++)
					{
						const vrl_eye & ve = u.sub.eye[e];
						auto it = chains.find(ve.swapchain);
						if (it == chains.end() || ve.image >= it->second->desc.image_count || !it->second->surface[ve.image] ||
						    !vrl_eye_rect_ok(ve, it->second->desc))
						{
							ok = false;
							continue;
						}
						const auto & d = it->second->desc;
						f.eye[e] = EyeSource{it->second->surface[ve.image], d.pixel_format, (d.flags & VRL_SWAPCHAIN_SRGB) != 0,
						                     ve.x, ve.y, ve.w, ve.h, (ve.flags & VRL_EYE_FLIP_Y) != 0};
						f.fov[e] = fov_ok ? EyeFov{u.sub.fov[e][0], u.sub.fov[e][1], u.sub.fov[e][2], u.sub.fov[e][3]} : EyeFov{};
					}
					if (ok)
					{
						try
						{
							sink(f);
							frames++;
						}
						catch (const std::exception & e)
						{
							fprintf(stderr, "vrhost: frame %llu dropped: %s\n", (unsigned long long)f.frame_id, e.what());
							if (on_sink_error)
								on_sink_error();
						}
						catch (...)
						{
							fprintf(stderr, "vrhost: frame %llu dropped: unknown error\n", (unsigned long long)f.frame_id);
							if (on_sink_error)
								on_sink_error();
						}
					}
					for (int e = 0; e < 2; e++)
						send_release(u.sub.eye[e].swapchain, u.sub.eye[e].image, u.sub.frame_id);
					break;
				}
				default:
					if (other)
						other(&u, size_t(n));
					break;
			}
		}
	}
};

VrLinkServer::VrLinkServer(int link_fd, const std::string & mach_name, Sink sink, MessageHandler other, SinkError on_sink_error) :
        p(std::make_unique<impl>())
{
	p->fd = link_fd;
	p->sink = std::move(sink);
	p->other = std::move(other);
	p->on_sink_error = std::move(on_sink_error);
	int on = 1;
	setsockopt(link_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
	int space = VRL_SOCKET_BUFFER;
	setsockopt(link_fd, SOL_SOCKET, SO_RCVBUF, &space, sizeof space);
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p->port) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), p->port, p->port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		throw std::runtime_error("mach port");
	// same mechanism as DXMT's WMTBootstrapRegister (bootstrap_register2 is deprecated but still served
	// for per-session names)
	if (bootstrap_register2(bootstrap_port, const_cast<char *>(mach_name.c_str()), p->port, 0) != KERN_SUCCESS)
		throw std::runtime_error("bootstrap_register2 " + mach_name);
	struct vrl_hello hello = {{VRL_HELLO, sizeof hello}, VRL_VERSION, uint32_t(getpid())};
	::send(link_fd, &hello, sizeof hello, 0);
	p->thread = std::thread([this] { p->run(); });
}

VrLinkServer::~VrLinkServer()
{
	p->quit = true;
	if (p->thread.joinable())
		p->thread.join();
	close(p->fd);
	p->chains.clear();
	if (p->port)
		mach_port_mod_refs(mach_task_self(), p->port, MACH_PORT_RIGHT_RECEIVE, -1);
}

bool VrLinkServer::send(const void * msg, size_t size)
{
	// MSG_DONTWAIT: a stalled vm side (full socket buffer) must never stall the host's frame clock
	ssize_t n = ::send(p->fd, msg, size, MSG_DONTWAIT);
	if (n < 0 && (errno == EPIPE || errno == ENOTCONN || errno == ECONNRESET))
		p->closed = true;
	return n == ssize_t(size);
}

uint64_t VrLinkServer::frames() const
{
	return p->frames;
}

bool VrLinkServer::peer_closed() const
{
	return p->closed;
}

int VrLinkServer::accept_one(const std::string & path, int timeout_ms)
{
	// listener for wmtr_vr_connect: hands over one end of a SOCK_DGRAM socketpair (macOS has no AF_UNIX
	// SOCK_SEQPACKET), keeps the other
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
	unlink(path.c_str());
	if (ls < 0 || bind(ls, (sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0)
	{
		if (ls >= 0)
			close(ls);
		return -1;
	}
	pollfd pf{ls, POLLIN, 0};
	int cs = poll(&pf, 1, timeout_ms) == 1 ? accept(ls, nullptr, nullptr) : -1;
	close(ls);
	unlink(path.c_str());
	if (cs < 0)
		return -1;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
	{
		close(cs);
		return -1;
	}
	char byte = 0;
	iovec iov{&byte, 1};
	union
	{
		cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} c{};
	msghdr m{};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = c.buf;
	m.msg_controllen = sizeof c.buf;
	cmsghdr * ch = CMSG_FIRSTHDR(&m);
	ch->cmsg_level = SOL_SOCKET;
	ch->cmsg_type = SCM_RIGHTS;
	ch->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(ch), &sv[1], sizeof(int));
	bool ok = sendmsg(cs, &m, 0) == 1;
	close(sv[1]);
	close(cs);
	if (!ok)
	{
		close(sv[0]);
		return -1;
	}
	return sv[0];
}

CompositorBridge::CompositorBridge(uint32_t out_w, uint32_t out_h, EyeEncoderSink & encoder, CompositorOptions opt) :
        compositor(out_w, out_h, opt), encoder(encoder)
{
}

void CompositorBridge::eye_offsets(const LinkFrame & f, float eye_x[2])
{
	// The eye positions are in stage space; their distance does not depend on where the head looks, their stage x
	// difference does (0 at 90 degrees yaw), and the foveation centre must not move with the head.
	float d2 = 0;
	for (int i = 4; i < 7; i++)
		d2 += (f.pose[1][i] - f.pose[0][i]) * (f.pose[1][i] - f.pose[0][i]);
	float half = std::sqrt(d2) / 2;
	eye_x[0] = -half;
	eye_x[1] = half;
}

void CompositorBridge::operator()(const LinkFrame & f)
{
	CVPixelBufferRef out[2] = {encoder.acquire(0), encoder.acquire(1)};
	FoveationParams params[2];
	float eye_x[2];
	eye_offsets(f, eye_x);
	int64_t t0 = now_ns();
	bool ok = out[0] && out[1] && compositor.compose(f.eye, f.fov, eye_x, out, params);
	int64_t t1 = now_ns();
	if (ok)
		for (int e = 0; e < 2; e++)
			encoder.encode(e, out[e], f, params[e]);
	int64_t t2 = now_ns();
	timing = {t1 - t0, compositor.last_gpu_ns(), t2 - t1};
	for (auto b: out)
		if (b)
			CVPixelBufferRelease(b);
}
} // namespace vrhost
