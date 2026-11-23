/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lan.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <memory>
#define CLOSE_SOCKET closesocket
using native_socket = SOCKET;
static const native_socket kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET ::close
using native_socket = int;
static const native_socket kBadSocket = -1;
#endif

namespace lan {

namespace {

bool is_private(uint32_t a)
{
	return (a >> 24) == 10 ||                 // 10.0.0.0/8
	       (a >> 20) == ((172u << 4) | 1u) || // 172.16.0.0/12
	       (a >> 16) == ((192u << 8) | 168u); // 192.168.0.0/16
}

bool set_nonblocking(native_socket fd)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool connect_in_progress()
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

#ifdef _WIN32
int poll_sockets(WSAPOLLFD *fds, size_t count, int timeout_ms)
{
	return WSAPoll(fds, (ULONG)count, timeout_ms);
}
using pollfd_t = WSAPOLLFD;
#else
int poll_sockets(pollfd *fds, size_t count, int timeout_ms)
{
	return ::poll(fds, (nfds_t)count, timeout_ms);
}
using pollfd_t = pollfd;
#endif

} // namespace

std::string to_string(uint32_t address)
{
	return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 0xFF) + "." +
	       std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

bool is_hotspot_client(const Interface &iface)
{
	return (iface.address & 0xFFFFFFF0u) == (kHotspotGateway & 0xFFFFFFF0u) && iface.address != kHotspotGateway;
}

std::vector<Interface> private_ipv4_interfaces()
{
	std::vector<Interface> result;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::unique_ptr<uint8_t[]> buffer;
	ULONG status = ERROR_BUFFER_OVERFLOW;
	for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; attempt++) {
		buffer.reset(new uint8_t[size]);
		status = GetAdaptersAddresses(
			AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
			reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.get()), &size);
	}
	if (status != NO_ERROR)
		return result;
	for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.get()); adapter; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
			const auto *sin = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
			if (sin->sin_family != AF_INET)
				continue;
			Interface iface;
			iface.address = ntohl(sin->sin_addr.s_addr);
			const unsigned prefix = unicast->OnLinkPrefixLength;
			iface.netmask = prefix == 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1));
			char name[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name, sizeof(name) - 1, nullptr,
					    nullptr);
			iface.name = name;
			if (is_private(iface.address))
				result.push_back(iface);
		}
	}
#else
	ifaddrs *head = nullptr;
	if (getifaddrs(&head) != 0)
		return result;
	for (ifaddrs *entry = head; entry; entry = entry->ifa_next) {
		if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_netmask)
			continue;
		if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
			continue;
		Interface iface;
		iface.name = entry->ifa_name ? entry->ifa_name : "";
		iface.address = ntohl(reinterpret_cast<sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr);
		iface.netmask = ntohl(reinterpret_cast<sockaddr_in *>(entry->ifa_netmask)->sin_addr.s_addr);
		if (is_private(iface.address))
			result.push_back(iface);
	}
	freeifaddrs(head);
#endif
	return result;
}

std::vector<uint32_t> scan_targets(const Interface &iface)
{
	// Never scan more than the /24 around our own address: big corporate subnets would take
	// too long and the phone is almost always that close.
	const uint32_t mask = iface.netmask | 0xFFFFFF00u;
	const uint32_t network = iface.address & mask;
	const uint32_t broadcast = network | ~mask;
	std::vector<uint32_t> targets;
	for (uint32_t a = network + 1; a < broadcast; a++)
		if (a != iface.address)
			targets.push_back(a);
	return targets;
}

namespace {

/* One batch of parallel connects; appends the addresses that accepted to `open`. */
void scan_batch(const uint32_t *targets, size_t count, uint16_t port, int timeout_ms, const std::atomic<bool> &stop,
		std::vector<uint32_t> &open)
{
	struct Probe {
		native_socket fd;
		uint32_t address;
	};
	std::vector<Probe> probes;

	for (size_t t = 0; t < count; t++) {
		const uint32_t address = targets[t];
		native_socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd == kBadSocket)
			continue;
		if (!set_nonblocking(fd)) {
			CLOSE_SOCKET(fd);
			continue;
		}
		sockaddr_in sin = {};
		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);
		sin.sin_addr.s_addr = htonl(address);
		const int r = ::connect(fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin));
		if (r == 0) {
			open.push_back(address);
			CLOSE_SOCKET(fd);
		} else if (connect_in_progress()) {
			probes.push_back({fd, address});
		} else {
			CLOSE_SOCKET(fd);
		}
	}

	using clock = std::chrono::steady_clock;
	const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
	std::vector<pollfd_t> fds;
	while (!probes.empty() && !stop.load()) {
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
		if (left <= 0)
			break;
		fds.assign(probes.size(), pollfd_t{});
		for (size_t i = 0; i < probes.size(); i++) {
			fds[i].fd = probes[i].fd;
			fds[i].events = POLLOUT;
		}
		const int ready = poll_sockets(fds.data(), fds.size(), (int)std::min<long long>(left, 100));
		if (ready < 0)
			break;
		if (ready == 0)
			continue;
		std::vector<Probe> pending;
		for (size_t i = 0; i < probes.size(); i++) {
			if (!fds[i].revents) {
				pending.push_back(probes[i]);
				continue;
			}
			int so_error = 1;
#ifdef _WIN32
			int len = sizeof(so_error);
#else
			socklen_t len = sizeof(so_error);
#endif
			getsockopt(probes[i].fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len);
			if (so_error == 0 && (fds[i].revents & POLLOUT))
				open.push_back(probes[i].address);
			CLOSE_SOCKET(probes[i].fd);
		}
		probes.swap(pending);
	}
	for (const auto &probe : probes)
		CLOSE_SOCKET(probe.fd);
}

} // namespace

std::vector<uint32_t> scan_port(const std::vector<uint32_t> &targets, uint16_t port, int timeout_ms,
				const std::atomic<bool> &stop)
{
	// Batches keep the number of open sockets well under macOS's default limit of 256 files.
	constexpr size_t kBatch = 128;
	std::vector<uint32_t> open;
	for (size_t i = 0; i < targets.size() && !stop.load(); i += kBatch)
		scan_batch(targets.data() + i, std::min(kBatch, targets.size() - i), port, timeout_ms, stop, open);
	return open;
}

} // namespace lan
