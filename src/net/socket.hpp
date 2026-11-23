/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

#ifdef _WIN32
using socket_t = uintptr_t;
constexpr socket_t kInvalidSocket = ~(socket_t)0;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

/* Small blocking TCP / Unix-domain socket wrapper with timeouts and cancellation. */
class Socket {
public:
	Socket() = default;
	~Socket();
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
	Socket(Socket &&other) noexcept;
	Socket &operator=(Socket &&other) noexcept;

	static bool platform_init();
	static void platform_shutdown();

	bool connect_tcp(const std::string &host, uint16_t port, int timeout_ms, std::string &error);
	bool connect_unix(const std::string &path, std::string &error);

	bool send_all(const void *data, size_t size, std::string &error);

	/* 1 = readable, 0 = timeout, -1 = error */
	int wait_readable(int timeout_ms);

	/* Reads exactly `size` bytes. Gives up when `stop` becomes true or when no byte arrives
	   for `idle_timeout_ms`. */
	bool recv_exact(void *buffer, size_t size, const std::atomic<bool> &stop, int idle_timeout_ms,
			std::string &error);

	std::string peer_ip() const; // numeric address of the other end ("" if unknown)
	void configure_stream(); // TCP_NODELAY, larger receive buffer, send timeout
	void shutdown();         // unblocks a thread waiting on this socket
	void close();
	bool valid() const { return fd_ != kInvalidSocket; }

private:
	socket_t fd_ = kInvalidSocket;
};

std::string last_socket_error();

} // namespace net
