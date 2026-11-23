/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "lcp/protocol.hpp"

#include <obs.h>

#include <functional>
#include <memory>
#include <string>

/* Receives each decoded picture. The frame (and its plane pointers) is only valid during the call. */
using FrameSink = std::function<void(obs_source_frame &frame)>;

class VideoDecoder {
public:
	virtual ~VideoDecoder() = default;

	/* (Re)configures for a new VIDEO_CONFIG. */
	virtual bool configure(const lcp::VideoConfig &config, std::string &error) = 0;

	/* Decodes one Annex B access unit. Returns false on a decode error (the caller should then
	   wait for the next keyframe). */
	virtual bool decode(const uint8_t *data, size_t size, bool keyframe, uint64_t pts_us,
			    const FrameSink &sink) = 0;

	virtual std::string description() const = 0;

	static std::unique_ptr<VideoDecoder> create(bool prefer_hardware);
};

/* Fills the BT.601 / BT.709 color parameters of an OBS frame. */
inline void set_frame_color(obs_source_frame &frame, bool bt709, bool full_range)
{
	frame.full_range = full_range;
	video_format_get_parameters_for_format(bt709 ? VIDEO_CS_709 : VIDEO_CS_601,
					       full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, frame.format,
					       frame.color_matrix, frame.color_range_min, frame.color_range_max);
}
