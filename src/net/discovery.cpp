/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "discovery.hpp"

#include "lcp/protocol.hpp"

#ifdef LUMACAM_HAVE_DNSSD

#include <dns_sd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace discovery {

namespace {

struct Resolver {
	std::string name;
	DNSServiceRef ref = nullptr;
};

struct State {
	std::mutex mutex;
	std::map<std::string, Service> services;
	std::thread thread;
	std::atomic<bool> running{false};
	DNSServiceRef connection = nullptr;
	DNSServiceRef browser = nullptr;
	std::map<std::string, std::unique_ptr<Resolver>> resolvers; // browse thread only
};

State &state()
{
	static State s;
	return s;
}

std::string txt_value(uint16_t length, const unsigned char *txt, const char *key)
{
	uint8_t value_length = 0;
	const void *value = TXTRecordGetValuePtr(length, txt, key, &value_length);
	return value ? std::string(static_cast<const char *>(value), value_length) : std::string();
}

void DNSSD_API on_resolve(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error, const char *,
			  const char *host, uint16_t port_be, uint16_t txt_length, const unsigned char *txt,
			  void *context)
{
	auto *resolver = static_cast<Resolver *>(context);
	if (error != kDNSServiceErr_NoError || !host)
		return;
	Service service;
	service.name = resolver->name;
	service.host = host;
	service.port = ntohs(port_be);
	service.id = txt_value(txt_length, txt, "id");
	service.hint = txt_value(txt_length, txt, "hint");
	std::lock_guard<std::mutex> lock(state().mutex);
	state().services[resolver->name] = service;
}

void DNSSD_API on_browse(DNSServiceRef, DNSServiceFlags flags, uint32_t interface_index, DNSServiceErrorType error,
			 const char *name, const char *type, const char *domain, void *)
{
	if (error != kDNSServiceErr_NoError || !name)
		return;
	State &s = state();
	const std::string key = name;

	if (flags & kDNSServiceFlagsAdd) {
		if (s.resolvers.count(key))
			return;
		auto resolver = std::make_unique<Resolver>();
		resolver->name = key;
		resolver->ref = s.connection;
		if (DNSServiceResolve(&resolver->ref, kDNSServiceFlagsShareConnection, interface_index, name, type,
				      domain, on_resolve, resolver.get()) == kDNSServiceErr_NoError)
			s.resolvers[key] = std::move(resolver);
	} else {
		auto it = s.resolvers.find(key);
		if (it != s.resolvers.end()) {
			DNSServiceRefDeallocate(it->second->ref);
			s.resolvers.erase(it);
		}
		std::lock_guard<std::mutex> lock(s.mutex);
		s.services.erase(key);
	}
}

void run()
{
	State &s = state();
	const int fd = DNSServiceRefSockFD(s.connection);
	while (s.running.load()) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		timeval tv = {0, 300000};
		int r = select(fd + 1, &set, nullptr, nullptr, &tv);
		if (r > 0 && DNSServiceProcessResult(s.connection) != kDNSServiceErr_NoError)
			break;
		if (r < 0)
			break;
	}
}

} // namespace

bool available()
{
	return true;
}

void start()
{
	State &s = state();
	if (s.running.load())
		return;
	if (DNSServiceCreateConnection(&s.connection) != kDNSServiceErr_NoError)
		return;
	s.browser = s.connection;
	if (DNSServiceBrowse(&s.browser, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny,
			     lcp::kBonjourType, nullptr, on_browse, nullptr) != kDNSServiceErr_NoError) {
		DNSServiceRefDeallocate(s.connection);
		s.connection = nullptr;
		return;
	}
	s.running = true;
	s.thread = std::thread(run);
}

void stop()
{
	State &s = state();
	if (!s.running.exchange(false))
		return;
	if (s.thread.joinable())
		s.thread.join();
	// Deallocating the shared connection releases every subordinate ref (browser, resolvers).
	DNSServiceRefDeallocate(s.connection);
	s.connection = nullptr;
	s.browser = nullptr;
	s.resolvers.clear();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.services.clear();
}

std::vector<Service> services()
{
	State &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	std::vector<Service> list;
	for (const auto &entry : s.services)
		list.push_back(entry.second);
	return list;
}

} // namespace discovery

#else // !LUMACAM_HAVE_DNSSD

namespace discovery {

bool available()
{
	return false;
}

void start() {}

void stop() {}

std::vector<Service> services()
{
	return {};
}

} // namespace discovery

#endif
