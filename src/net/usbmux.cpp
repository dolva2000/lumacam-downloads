/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "usbmux.hpp"

#include <atomic>
#include <cstring>

namespace usbmux {

namespace {

constexpr uint32_t kVersionPlist = 1;
constexpr uint32_t kMessagePlist = 8;
constexpr size_t kHeaderSize = 16;
constexpr uint32_t kMaxReply = 4 * 1024 * 1024;

void write_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool open_daemon(net::Socket &socket, std::string &error)
{
#ifdef _WIN32
	return socket.connect_tcp("127.0.0.1", 27015, 1000, error);
#else
	return socket.connect_unix("/var/run/usbmuxd", error);
#endif
}

std::string plist(const std::string &body)
{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	       "<plist version=\"1.0\"><dict>" +
	       body +
	       "<key>ClientVersionString</key><string>lumacam-obs</string>"
	       "<key>ProgName</key><string>lumacam-obs</string>"
	       "</dict></plist>\n";
}

bool exchange(net::Socket &socket, const std::string &request, std::string &reply, std::string &error)
{
	std::vector<uint8_t> packet(kHeaderSize + request.size());
	write_le32(packet.data(), (uint32_t)packet.size());
	write_le32(packet.data() + 4, kVersionPlist);
	write_le32(packet.data() + 8, kMessagePlist);
	write_le32(packet.data() + 12, 1); // tag
	memcpy(packet.data() + kHeaderSize, request.data(), request.size());
	if (!socket.send_all(packet.data(), packet.size(), error))
		return false;

	std::atomic<bool> never_stop{false};
	uint8_t header[kHeaderSize];
	if (!socket.recv_exact(header, sizeof(header), never_stop, 3000, error))
		return false;
	uint32_t length = read_le32(header);
	if (length < kHeaderSize || length > kMaxReply) {
		error = "invalid usbmuxd reply";
		return false;
	}
	reply.assign(length - kHeaderSize, '\0');
	if (!reply.empty() && !socket.recv_exact(&reply[0], reply.size(), never_stop, 3000, error))
		return false;
	return true;
}

/* Value following <key>name</key> inside [begin, end): returns the element text. */
std::string value_after_key(const std::string &xml, const std::string &key, size_t begin, size_t end)
{
	const std::string tag = "<key>" + key + "</key>";
	size_t pos = xml.find(tag, begin);
	if (pos == std::string::npos || pos >= end)
		return {};
	pos += tag.size();
	size_t open = xml.find('<', pos);
	size_t open_end = xml.find('>', open);
	if (open == std::string::npos || open_end == std::string::npos || open_end >= end)
		return {};
	size_t close = xml.find("</", open_end);
	if (close == std::string::npos || close > end)
		return {};
	return xml.substr(open_end + 1, close - open_end - 1);
}

} // namespace

std::vector<Device> parse_device_list(const std::string &xml)
{
	std::vector<Device> devices;
	const std::string marker = "<key>Properties</key>";
	size_t pos = 0;
	while ((pos = xml.find(marker, pos)) != std::string::npos) {
		size_t dict_begin = xml.find("<dict>", pos);
		size_t dict_end = xml.find("</dict>", dict_begin);
		if (dict_begin == std::string::npos || dict_end == std::string::npos)
			break;

		Device device;
		device.connection_type = value_after_key(xml, "ConnectionType", dict_begin, dict_end);
		device.serial = value_after_key(xml, "SerialNumber", dict_begin, dict_end);
		const std::string id = value_after_key(xml, "DeviceID", dict_begin, dict_end);
		device.id = id.empty() ? 0 : atoi(id.c_str());
		if (device.id > 0)
			devices.push_back(device);
		pos = dict_end;
	}
	return devices;
}

int parse_result_number(const std::string &xml)
{
	const std::string number = value_after_key(xml, "Number", 0, xml.size());
	return number.empty() ? -1 : atoi(number.c_str());
}

bool list_devices(std::vector<Device> &devices, std::string &error)
{
	devices.clear();
	net::Socket socket;
	if (!open_daemon(socket, error))
		return false;

	std::string reply;
	if (!exchange(socket, plist("<key>MessageType</key><string>ListDevices</string>"), reply, error))
		return false;

	for (const Device &device : parse_device_list(reply)) {
		if (device.connection_type == "USB")
			devices.push_back(device);
	}
	return true;
}

bool connect(int device_id, uint16_t port, net::Socket &socket, std::string &error)
{
	if (!open_daemon(socket, error))
		return false;

	// usbmuxd expects the port in network byte order, stored in a host-order integer.
	const uint16_t swapped = (uint16_t)(((port & 0xFF) << 8) | (port >> 8));
	const std::string body = "<key>MessageType</key><string>Connect</string>"
				 "<key>DeviceID</key><integer>" +
				 std::to_string(device_id) +
				 "</integer>"
				 "<key>PortNumber</key><integer>" +
				 std::to_string(swapped) + "</integer>";

	std::string reply;
	if (!exchange(socket, plist(body), reply, error))
		return false;

	const int result = parse_result_number(reply);
	if (result != 0) {
		error = result == 3 ? "the phone refused the USB connection (is LumaCam open on the phone?)"
				    : "usbmuxd connect failed (code " + std::to_string(result) + ")";
		socket.close();
		return false;
	}
	socket.configure_stream();
	return true;
}

} // namespace usbmux
