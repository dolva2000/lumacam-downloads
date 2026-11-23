/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Finds the phone and opens a connection to it, whatever links the computer and the phone:
// USB cable (usbmuxd), same Wi-Fi network, iPhone Personal Hotspot (Wi-Fi or USB tethering),
// or a typed IP address. Over the network the phone is identified by the 4-digit pairing code
// shown in the app; USB needs no code. Shared by the OBS source and tools/probe.

#pragma once

#include "socket.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace connector {

enum class Route {
	Usb,     // usbmuxd tunnel
	Hotspot, // the phone is this computer's Personal Hotspot gateway
	Wifi,    // same local network (found with Bonjour or by scanning)
	Manual,  // IP address typed by the user
};

enum class Failure {
	None,
	NoPhone,       // nothing answered anywhere
	CodeNeeded,    // no phone on USB and no (valid) code to look on the network
	NoMatch,       // phones answered on the network, none with this code
	UsbNotFound,   // a specific USB device was selected and is not plugged in
	ManualFailed,  // the typed address did not answer
	Stopped,
};

struct Request {
	std::string mode = "auto"; // "auto" | "network" | "manual" | "usb:<serial>" | "wifi:<bonjour name>"
	std::string host;          // manual mode
	uint16_t port = 4848;
	std::string code;          // 4 digits, may be empty
	std::string remembered_host; // last address that worked, tried first
};

struct Identity {
	bool valid = false;
	std::string id, name, model, app, hint, pairing;
};

struct Result {
	Route route = Route::Usb;
	std::string host; // IP or host name ("" for USB)
	Identity identity;
};

/* Blocking; honours `stop`. On success `socket` is connected and IDENTITY has been read when
   the phone was reached over the network (the caller sends HELLO next, with the code). */
bool connect(const Request &request, net::Socket &socket, Result &result, Failure &failure, std::string &detail,
	     const std::atomic<bool> &stop);

/* Sends IDENTIFY and waits up to `timeout_ms` for IDENTITY. */
bool identify(net::Socket &socket, Identity &identity, int timeout_ms, const std::atomic<bool> &stop);

const char *route_name(Route route); // "usb", "hotspot", "wifi", "manual" (for logs)

} // namespace connector
