// SPDX-License-Identifier: GPL-3.0-or-later
#include "wivrn/dnssd_publisher.h"

#include <arpa/inet.h>
#include <atomic>
#include <dns_sd.h>
#include <poll.h>
#include <stdexcept>
#include <thread>

namespace vrhost
{
struct DnssdPublisher::impl
{
	DNSServiceRef ref = nullptr;
	std::atomic<bool> ok{false};
	std::atomic<bool> quit{false};
	std::thread thread;
};

static void DNSSD_API on_register(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char *, const char *, const char *, void * ctx)
{
	static_cast<std::atomic<bool> *>(ctx)->store(err == kDNSServiceErr_NoError);
}

DnssdPublisher::DnssdPublisher(const std::string & name, const std::string & type, int port, const std::map<std::string, std::string> & txt) :
        p(std::make_unique<impl>())
{
	TXTRecordRef rec;
	TXTRecordCreate(&rec, 0, nullptr);
	for (auto & [k, v]: txt)
		TXTRecordSetValue(&rec, k.c_str(), uint8_t(v.size()), v.data());
	DNSServiceErrorType err = DNSServiceRegister(&p->ref, 0, kDNSServiceInterfaceIndexAny, name.empty() ? nullptr : name.c_str(),
	                                             type.c_str(), nullptr, nullptr, htons(uint16_t(port)),
	                                             TXTRecordGetLength(&rec), TXTRecordGetBytesPtr(&rec), on_register, &p->ok);
	TXTRecordDeallocate(&rec);
	if (err != kDNSServiceErr_NoError)
		throw std::runtime_error("DNSServiceRegister failed: " + std::to_string(err));
	p->thread = std::thread([this] {
		int fd = DNSServiceRefSockFD(p->ref);
		while (not p->quit)
		{
			pollfd pfd{.fd = fd, .events = POLLIN};
			if (::poll(&pfd, 1, 100) > 0)
				DNSServiceProcessResult(p->ref);
		}
	});
}

DnssdPublisher::~DnssdPublisher()
{
	p->quit = true;
	if (p->thread.joinable())
		p->thread.join();
	DNSServiceRefDeallocate(p->ref);
}

bool DnssdPublisher::registered() const
{
	return p->ok;
}
} // namespace vrhost
