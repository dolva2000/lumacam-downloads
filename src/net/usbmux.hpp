/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Minimal usbmuxd client: lists iOS devices attached over USB and opens a TCP tunnel to a
// port on the device. usbmuxd ships with macOS, with iTunes / Apple Devices on Windows
// (Apple Mobile Device Service) and as the `usbmuxd` package on Linux.

#pragma once

#include "socket.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace usbmux {

struct Device {
	int id = 0;
	std::string serial;
	std::string connection_type; // "USB" or "Network"
};

/* Lists devices connected over USB (network-paired devices are skipped). */
bool list_devices(std::vector<Device> &devices, std::string &error);

/* Opens a tunnel to `port` on the device; on success `socket` is a raw byte stream. */
bool connect(int device_id, uint16_t port, net::Socket &socket, std::string &error);

/* Exposed for tests. */
std::vector<Device> parse_device_list(const std::string &plist);
int parse_result_number(const std::string &plist);

} // namespace usbmux
