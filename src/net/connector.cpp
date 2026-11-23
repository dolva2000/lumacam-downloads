/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "connector.hpp"

#include "discovery.hpp"
#include "lan.hpp"
#include "usbmux.hpp"
#include "lcp/protocol.hpp"

#include <chrono>
#include <cstdlib>
#include <set>
#include <vector>

namespace connector {

namespace {

constexpr int kConnectTimeoutMs = 1500;
constexpr int kIdentifyTimeoutMs = 1500;
constexpr int kScanTimeoutMs = 800;

bool starts_with(const std::string &s, const char *prefix)
{
	return s.rfind(prefix, 0) == 0;
}

void append_utf8(std::string &out, uint32_t cp)
{
	if (cp < 0x80) {
		out += (char)cp;
	} else if (cp < 0x800) {
		out += (char)(0xC0 | (cp >> 6));
		out += (char)(0x80 | (cp & 0x3F));
	} else {
		out += (char)(0xE0 | (cp >> 12));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	}
}

/* Reads a top-level string field from a flat JSON object (IDENTITY only has strings and one
   integer), so this module does not depend on libobs. */
std::string json_string(const std::string &json, const char *key)
{
	const std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos)
		return {};
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos)
		return {};
	pos = json.find_first_not_of(" \t\r\n", pos + 1);
	if (pos == std::string::npos || json[pos] != '"')
		return {};

	std::string out;
	for (size_t i = pos + 1; i < json.size(); i++) {
		const char c = json[i];
		if (c == '"')
			return out;
		if (c != '\\') {
			out += c;
			continue;
		}
		if (++i >= json.size())
			break;
		switch (json[i]) {
		case 'n':
			out += '\n';
			break;
		case 't':
			out += '\t';
			break;
		case 'u':
			if (i + 4 < json.size()) {
				append_utf8(out, (uint32_t)strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
				i += 4;
			}
			break;
		default: // \" \\ \/
			out += json[i];
			break;
		}
	}
	return {};
}

Route route_for(const std::string &host)
{
	return host == lan::to_string(lan::kHotspotGateway) ? Route::Hotspot : Route::Wifi;
}

} // namespace

const char *route_name(Route route)
{
	switch (route) {
	case Route::Usb:
		return "usb";
	case Route::Hotspot:
		return "hotspot";
	case Route::Wifi:
		return "wifi";
	case Route::Manual:
		return "manual";
	}
	return "?";
}

bool identify(net::Socket &socket, Identity &identity, int timeout_ms, const std::atomic<bool> &stop)
{
	identity = Identity();
	const auto packet = lcp::build_packet(lcp::MessageType::Identify, 0, 0, nullptr, 0);
	std::string error;
	if (!socket.send_all(packet.data(), packet.size(), error))
		return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	uint8_t header_bytes[lcp::kHeaderSize];
	std::vector<uint8_t> payload;
	while (std::chrono::steady_clock::now() < deadline && !stop.load()) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
					  deadline - std::chrono::steady_clock::now())
					  .count();
		lcp::Header header;
		if (!socket.recv_exact(header_bytes, sizeof(header_bytes), stop, (int)left, error) ||
		    !lcp::decode_header(header_bytes, header, error))
			return false;
		payload.resize(header.length);
		if (header.length && !socket.recv_exact(payload.data(), payload.size(), stop, kIdentifyTimeoutMs, error))
			return false;
		if ((lcp::MessageType)header.type != lcp::MessageType::Identity)
			continue;
		const std::string json(payload.begin(), payload.end());
		identity.valid = true;
		identity.id = json_string(json, "id");
		identity.name = json_string(json, "name");
		identity.model = json_string(json, "model");
		identity.app = json_string(json, "app");
		identity.hint = json_string(json, "hint");
		identity.pairing = json_string(json, "pairing");
		return true;
	}
	return false;
}

bool connect(const Request &request, net::Socket &socket, Result &result, Failure &failure, std::string &detail,
	     const std::atomic<bool> &stop)
{
	failure = Failure::None;
	detail.clear();
	result = Result();
	const uint16_t port = request.port ? request.port : lcp::kDefaultPort;
	const std::string code = lcp::normalize_code(request.code);
	const bool have_code = lcp::is_valid_code(code);
	const std::string hint = have_code ? lcp::pairing_hint(code) : std::string();

	/* Manual address: connect as told, the phone checks the code in HELLO. */
	if (request.mode == "manual") {
		if (request.host.empty() || !socket.connect_tcp(request.host, port, 3000, detail)) {
			failure = Failure::ManualFailed;
			return false;
		}
		result.route = Route::Manual;
		result.host = request.host;
		if (!identify(socket, result.identity, kIdentifyTimeoutMs, stop)) {
			// Old app without IDENTIFY: start over on a fresh connection.
			if (!socket.connect_tcp(request.host, port, 3000, detail)) {
				failure = Failure::ManualFailed;
				return false;
			}
		}
		return true;
	}

	const bool any = request.mode == "auto";
	const bool network_only = request.mode == "network";

	/* 1. USB: the cable is proof of physical access, no code needed. */
	if (!network_only && (any || starts_with(request.mode, "usb:"))) {
		const std::string serial = any ? std::string() : request.mode.substr(4);
		std::vector<usbmux::Device> devices;
		std::string usb_error;
		if (usbmux::list_devices(devices, usb_error)) {
			for (const auto &device : devices) {
				if (!serial.empty() && device.serial != serial)
					continue;
				if (usbmux::connect(device.id, port, socket, usb_error)) {
					result.route = Route::Usb;
					return true;
				}
			}
		}
		if (!any) {
			failure = Failure::UsbNotFound;
			detail = usb_error;
			return false;
		}
	}
	if (stop.load()) {
		failure = Failure::Stopped;
		return false;
	}

	/* 2. Network: only with a code, and only the phone whose fingerprint matches it. */
	if (!have_code) {
		failure = Failure::CodeNeeded;
		return false;
	}

	std::set<std::string> tried;
	bool saw_other_phone = false;
	auto attempt = [&](const std::string &host, uint16_t p, Route route) -> bool {
		if (stop.load() || !tried.insert(host + ":" + std::to_string(p)).second)
			return false;
		std::string error;
		if (!socket.connect_tcp(host, p, kConnectTimeoutMs, error))
			return false;
		Identity identity;
		if (!identify(socket, identity, kIdentifyTimeoutMs, stop)) {
			socket.close();
			return false;
		}
		if (identity.hint != hint) {
			saw_other_phone = true;
			socket.close();
			return false;
		}
		// Report the numeric address actually reached ("iPhone.local." may be the hotspot).
		const std::string peer = socket.peer_ip();
		result.host = peer.empty() ? host : peer;
		result.route = route == Route::Wifi ? route_for(result.host) : route;
		result.identity = identity;
		return true;
	};

	const bool wifi_only = starts_with(request.mode, "wifi:");
	const std::string wanted_name = wifi_only ? request.mode.substr(5) : std::string();

	// 2a. The address that worked last time: reconnects in a few milliseconds.
	if (!wifi_only && !request.remembered_host.empty() &&
	    attempt(request.remembered_host, port, route_for(request.remembered_host)))
		return true;

	// 2b. Bonjour (same Wi-Fi, or Personal Hotspot over Wi-Fi).
	for (const auto &service : discovery::services()) {
		if (wifi_only && service.name != wanted_name)
			continue;
		if (!service.hint.empty() && service.hint != hint) {
			saw_other_phone = true;
			continue;
		}
		if (attempt(service.host, service.port, Route::Wifi))
			return true;
	}

	if (!wifi_only) {
		const auto interfaces = lan::private_ipv4_interfaces();

		// 2c. Personal Hotspot: the iPhone is always the gateway, 172.20.10.1.
		for (const auto &iface : interfaces) {
			if (lan::is_hotspot_client(iface) &&
			    attempt(lan::to_string(lan::kHotspotGateway), port, Route::Hotspot))
				return true;
		}

		// 2d. Last resort when Bonjour is blocked or missing (some routers, Windows without
		//     Bonjour): scan the local subnet for the LumaCam port.
		for (const auto &iface : interfaces) {
			if (stop.load())
				break;
			std::vector<uint32_t> targets;
			for (uint32_t address : lan::scan_targets(iface))
				if (!tried.count(lan::to_string(address) + ":" + std::to_string(port)))
					targets.push_back(address);
			for (uint32_t address : lan::scan_port(targets, port, kScanTimeoutMs, stop)) {
				const std::string host = lan::to_string(address);
				if (attempt(host, port, route_for(host)))
					return true;
			}
		}
	}

	failure = stop.load() ? Failure::Stopped : (saw_other_phone ? Failure::NoMatch : Failure::NoPhone);
	return false;
}

} // namespace connector
