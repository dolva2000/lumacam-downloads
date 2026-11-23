/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// LumaCam Protocol (LCP) v1 — see docs/PROTOCOL.md at the repository root.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lcp {

constexpr uint16_t kMagic = 0x4C4D; // "LM"
constexpr size_t kHeaderSize = 16;
constexpr uint32_t kMaxPayload = 16u * 1024u * 1024u;
constexpr uint16_t kDefaultPort = 4848;
constexpr int kVersion = 1;
constexpr const char *kBonjourType = "_lumacam._tcp";

enum class MessageType : uint8_t {
	Hello = 0x01,
	ServerInfo = 0x02,
	Identify = 0x03,
	Identity = 0x04,
	VideoConfig = 0x10,
	VideoFrame = 0x11,
	Control = 0x30,
	Status = 0x31,
	Ping = 0x40,
	Pong = 0x41,
	Error = 0x7F,
};

constexpr uint8_t kFlagKeyframe = 0x01;

/* Pairing (docs/PROTOCOL.md §3.2) ----------------------------------------- */

constexpr size_t kCodeLength = 4;

/* Keeps the digits of what the user typed ("48 21" -> "4821"). */
inline std::string normalize_code(const std::string &text)
{
	std::string digits;
	for (char c : text)
		if (c >= '0' && c <= '9')
			digits += c;
	return digits;
}

inline bool is_valid_code(const std::string &code)
{
	return code.size() == kCodeLength && normalize_code(code) == code;
}

/* Public 2-hex-digit fingerprint of a pairing code, published by the phone in Bonjour and
   IDENTITY: FNV-1a 32-bit of "lumacam:" + code, low byte. Must match Pairing.hint in the app. */
inline std::string pairing_hint(const std::string &code)
{
	uint32_t hash = 2166136261u;
	const std::string input = "lumacam:" + code;
	for (unsigned char c : input) {
		hash ^= c;
		hash *= 16777619u;
	}
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out += hex[(hash >> 4) & 0xF];
	out += hex[hash & 0xF];
	return out;
}

struct Header {
	uint8_t type = 0;
	uint8_t flags = 0;
	uint32_t length = 0;
	uint64_t timestamp = 0;
};

enum class Codec : uint8_t {
	H264 = 1,
	HEVC = 2,
};

struct VideoConfig {
	Codec codec = Codec::HEVC;
	uint8_t bit_depth = 8;
	bool full_range = false;
	uint8_t colorspace = 1; // 1 = BT.709, 2 = BT.2020
	uint16_t width = 0;
	uint16_t height = 0;
	uint16_t fps = 0;
	std::vector<uint8_t> parameter_sets; // Annex B

	bool operator==(const VideoConfig &o) const
	{
		return codec == o.codec && bit_depth == o.bit_depth && full_range == o.full_range &&
		       colorspace == o.colorspace && width == o.width && height == o.height &&
		       parameter_sets == o.parameter_sets;
	}
	bool operator!=(const VideoConfig &o) const { return !(*this == o); }
};

inline uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

inline uint64_t read_be64(const uint8_t *p)
{
	return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

inline void write_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

inline void write_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

inline void write_be64(uint8_t *p, uint64_t v)
{
	write_be32(p, (uint32_t)(v >> 32));
	write_be32(p + 4, (uint32_t)v);
}

inline void encode_header(const Header &h, uint8_t out[kHeaderSize])
{
	write_be16(out, kMagic);
	out[2] = h.type;
	out[3] = h.flags;
	write_be32(out + 4, h.length);
	write_be64(out + 8, h.timestamp);
}

inline bool decode_header(const uint8_t in[kHeaderSize], Header &out, std::string &error)
{
	if (read_be16(in) != kMagic) {
		error = "invalid packet magic";
		return false;
	}
	out.type = in[2];
	out.flags = in[3];
	out.length = read_be32(in + 4);
	out.timestamp = read_be64(in + 8);
	if (out.length > kMaxPayload) {
		error = "packet too large";
		return false;
	}
	return true;
}

inline std::vector<uint8_t> build_packet(MessageType type, uint8_t flags, uint64_t timestamp, const void *payload,
					 size_t size)
{
	std::vector<uint8_t> packet(kHeaderSize + size);
	Header h;
	h.type = (uint8_t)type;
	h.flags = flags;
	h.length = (uint32_t)size;
	h.timestamp = timestamp;
	encode_header(h, packet.data());
	if (size)
		memcpy(packet.data() + kHeaderSize, payload, size);
	return packet;
}

inline bool parse_video_config(const uint8_t *data, size_t size, VideoConfig &out)
{
	if (size < 12)
		return false;
	if (data[0] != (uint8_t)Codec::H264 && data[0] != (uint8_t)Codec::HEVC)
		return false;
	out.codec = (Codec)data[0];
	out.bit_depth = data[1];
	out.full_range = data[2] == 1;
	out.colorspace = data[3];
	out.width = read_be16(data + 4);
	out.height = read_be16(data + 6);
	out.fps = read_be16(data + 8);
	out.parameter_sets.assign(data + 12, data + size);
	return out.width > 0 && out.height > 0 && !out.parameter_sets.empty();
}

/* Annex B helpers ------------------------------------------------------- */

struct NalUnit {
	const uint8_t *data;
	size_t size;
};

/* Splits an Annex B byte stream (3- or 4-byte start codes) into NAL units. */
inline std::vector<NalUnit> split_annexb(const uint8_t *data, size_t size)
{
	std::vector<NalUnit> units;
	size_t i = 0;
	size_t start = SIZE_MAX;
	while (i + 2 < size) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			if (start != SIZE_MAX) {
				size_t end = i;
				if (end > start && data[end - 1] == 0)
					end--;
				units.push_back({data + start, end - start});
			}
			i += 3;
			start = i;
		} else {
			i++;
		}
	}
	if (start != SIZE_MAX && start < size)
		units.push_back({data + start, size - start});
	return units;
}

inline uint8_t nal_type(Codec codec, const NalUnit &nal)
{
	if (nal.size == 0)
		return 0xFF;
	return codec == Codec::HEVC ? (uint8_t)((nal.data[0] >> 1) & 0x3F) : (uint8_t)(nal.data[0] & 0x1F);
}

/* True for VPS/SPS/PPS and access unit delimiters, which decoders take out of band. */
inline bool is_out_of_band_nal(Codec codec, uint8_t type)
{
	if (codec == Codec::HEVC)
		return type == 32 || type == 33 || type == 34 || type == 35;
	return type == 7 || type == 8 || type == 9;
}

} // namespace lcp
