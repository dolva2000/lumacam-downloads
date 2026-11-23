/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Local network helpers used to find the phone without Bonjour: the computer's IPv4
// interfaces, the iPhone Personal Hotspot gateway, and a fast parallel port scan of the subnet.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lan {

struct Interface {
	std::string name;
	uint32_t address = 0; // host byte order
	uint32_t netmask = 0; // host byte order
};

/* Up, non-loopback IPv4 interfaces with a private (RFC 1918) address. */
std::vector<Interface> private_ipv4_interfaces();

/* iPhone Personal Hotspot always uses 172.20.10.0/28 and is the gateway at 172.20.10.1. */
constexpr uint32_t kHotspotGateway = (172u << 24) | (20u << 16) | (10u << 8) | 1u;
bool is_hotspot_client(const Interface &iface);

/* Addresses worth scanning on this interface's subnet (at most the /24 around the computer's
   own address), excluding the computer itself. */
std::vector<uint32_t> scan_targets(const Interface &iface);

/* Tries a TCP connection to `port` on every target in parallel and returns those that accept,
   in the order they answered. Gives up after `timeout_ms` or when `stop` becomes true. */
std::vector<uint32_t> scan_port(const std::vector<uint32_t> &targets, uint16_t port, int timeout_ms,
				const std::atomic<bool> &stop);

std::string to_string(uint32_t address);

} // namespace lan
