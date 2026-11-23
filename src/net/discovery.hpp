/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Bonjour / mDNS browser for `_lumacam._tcp` services (phones on the local network).
// Uses dns_sd (built into macOS; Avahi compat on Linux; Bonjour SDK on Windows).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace discovery {

struct Service {
	std::string name; // e.g. "iPhone de Marie"
	std::string host; // e.g. "iPhone-de-Marie.local."
	uint16_t port = 0;
	std::string id;   // stable phone id (TXT "id"), empty for old app versions
	std::string hint; // pairing code fingerprint (TXT "hint"), see lcp::pairing_hint
};

bool available();
void start();
void stop();
std::vector<Service> services();

} // namespace discovery
