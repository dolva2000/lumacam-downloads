/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "socket.hpp"

#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_len = int;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using sock_len = socklen_t;
#define CLOSE_SOCKET ::close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace net {

namespace {

bool set_blocking(socket_t fd, bool blocking)
{
#ifdef _WIN32
	u_long mode = blocking ? 0 : 1;
	return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;
	flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
	return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool would_block()
{
#ifdef _WIN32
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
	return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

int select_fd(socket_t fd, bool write, int timeout_ms)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(fd, &set);
	timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int nfds = (int)fd + 1;
	int r = write ? select(nfds, nullptr, &set, nullptr, &tv) : select(nfds, &set, nullptr, nullptr, &tv);
	if (r < 0)
		return -1;
	return r > 0 ? 1 : 0;
}

} // namespace

std::string last_socket_error()
{
#ifdef _WIN32
	return "socket error " + std::to_string(WSAGetLastError());
#else
	return strerror(errno);
#endif
}

Socket::~Socket()
{
	close();
}

Socket::Socket(Socket &&other) noexcept : fd_(other.fd_)
{
	other.fd_ = kInvalidSocket;
}

Socket &Socket::operator=(Socket &&other) noexcept
{
	if (this != &other) {
		close();
		fd_ = other.fd_;
		other.fd_ = kInvalidSocket;
	}
	return *this;
}

bool Socket::platform_init()
{
#ifdef _WIN32
	WSADATA data;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
	return true;
#endif
}

void Socket::platform_shutdown()
{
#ifdef _WIN32
	WSACleanup();
#endif
}

bool Socket::connect_tcp(const std::string &host, uint16_t port, int timeout_ms, std::string &error)
{
	close();

	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo *result = nullptr;
	const std::string service = std::to_string(port);
	int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
	if (gai != 0 || !result) {
		error = "cannot resolve " + host;
		return false;
	}

	// IPv4 first: phones on a LAN or Personal Hotspot always have one, and IPv6 link-local
	// addresses (fe80::) are unreadable and cannot be reused without their interface scope.
	// Order: routable IPv4, link-local IPv4 (169.254/16), then IPv6.
	auto rank = [](const addrinfo *ai) {
		if (ai->ai_family != AF_INET)
			return 2;
		const uint32_t a = ntohl(((const sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
		return (a >> 16) == 0xA9FE ? 1 : 0;
	};
	std::vector<addrinfo *> candidates;
	for (int pass = 0; pass < 3; pass++)
		for (addrinfo *ai = result; ai; ai = ai->ai_next)
			if (rank(ai) == pass)
				candidates.push_back(ai);

	error = "cannot connect to " + host + ":" + service;
	for (addrinfo *ai : candidates) {
		socket_t fd = (socket_t)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == kInvalidSocket)
			continue;

		set_blocking(fd, false);
		int r = ::connect(fd, ai->ai_addr, (sock_len)ai->ai_addrlen);
		bool ok = r == 0;
		if (!ok && would_block() && select_fd(fd, true, timeout_ms) == 1) {
			int so_error = 0;
			sock_len len = sizeof(so_error);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
			ok = so_error == 0;
		}
		if (ok) {
			set_blocking(fd, true);
			fd_ = fd;
			configure_stream();
			freeaddrinfo(result);
			error.clear();
			return true;
		}
		CLOSE_SOCKET(fd);
	}

	freeaddrinfo(result);
	return false;
}

bool Socket::connect_unix(const std::string &path, std::string &error)
{
#ifdef _WIN32
	(void)path;
	error = "unix sockets are not supported on Windows";
	return false;
#else
	close();
	socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == kInvalidSocket) {
		error = last_socket_error();
		return false;
	}
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
	if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
		error = "cannot open " + path + ": " + last_socket_error();
		CLOSE_SOCKET(fd);
		return false;
	}
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	fd_ = fd;
	return true;
#endif
}

std::string Socket::peer_ip() const
{
	if (!valid())
		return {};
	sockaddr_storage addr = {};
	sock_len len = sizeof(addr);
	if (getpeername(fd_, (sockaddr *)&addr, &len) != 0)
		return {};
	char text[INET6_ADDRSTRLEN] = {};
	if (addr.ss_family == AF_INET) {
		inet_ntop(AF_INET, &((sockaddr_in *)&addr)->sin_addr, text, sizeof(text));
	} else if (addr.ss_family == AF_INET6) {
		auto *sin6 = (sockaddr_in6 *)&addr;
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
			inet_ntop(AF_INET, &sin6->sin6_addr.s6_addr[12], text, sizeof(text));
		else
			inet_ntop(AF_INET6, &sin6->sin6_addr, text, sizeof(text));
	}
	return text;
}

void Socket::configure_stream()
{
	if (!valid())
		return;
	int one = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
#ifdef SO_NOSIGPIPE
	setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#endif
#ifdef _WIN32
	DWORD timeout = 3000;
	setsockopt((SOCKET)fd_, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
	timeval timeout = {3, 0};
	setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool Socket::send_all(const void *data, size_t size, std::string &error)
{
	const char *p = (const char *)data;
	while (size > 0) {
		if (!valid()) {
			error = "socket closed";
			return false;
		}
		int chunk = size > (1u << 30) ? (1 << 30) : (int)size;
		int sent = (int)::send(fd_, p, chunk, MSG_NOSIGNAL);
		if (sent <= 0) {
			error = "send failed: " + last_socket_error();
			return false;
		}
		p += sent;
		size -= (size_t)sent;
	}
	return true;
}

int Socket::wait_readable(int timeout_ms)
{
	if (!valid())
		return -1;
	return select_fd(fd_, false, timeout_ms);
}

bool Socket::recv_exact(void *buffer, size_t size, const std::atomic<bool> &stop, int idle_timeout_ms,
			std::string &error)
{
	using clock = std::chrono::steady_clock;
	char *p = (char *)buffer;
	auto last_data = clock::now();

	while (size > 0) {
		if (stop.load()) {
			error = "stopped";
			return false;
		}
		int ready = wait_readable(200);
		if (ready < 0) {
			error = "connection error";
			return false;
		}
		if (ready == 0) {
			auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - last_data);
			if (idle.count() > idle_timeout_ms) {
				error = "connection timed out";
				return false;
			}
			continue;
		}
		int chunk = size > (1u << 30) ? (1 << 30) : (int)size;
		int got = (int)::recv(fd_, p, chunk, 0);
		if (got == 0) {
			error = "connection closed by phone";
			return false;
		}
		if (got < 0) {
			error = "receive failed: " + last_socket_error();
			return false;
		}
		p += got;
		size -= (size_t)got;
		last_data = clock::now();
	}
	return true;
}

void Socket::shutdown()
{
	if (!valid())
		return;
#ifdef _WIN32
	::shutdown((SOCKET)fd_, SD_BOTH);
#else
	::shutdown(fd_, SHUT_RDWR);
#endif
}

void Socket::close()
{
	if (!valid())
		return;
	CLOSE_SOCKET(fd_);
	fd_ = kInvalidSocket;
}

} // namespace net
