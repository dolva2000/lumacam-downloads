/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "video-decoder.hpp"

#ifdef __APPLE__
std::unique_ptr<VideoDecoder> make_videotoolbox_decoder();
#endif
#ifdef LUMACAM_HAVE_FFMPEG
std::unique_ptr<VideoDecoder> make_ffmpeg_decoder(bool prefer_hardware);
#endif

std::unique_ptr<VideoDecoder> VideoDecoder::create(bool prefer_hardware)
{
#if defined(__APPLE__)
#if defined(LUMACAM_HAVE_FFMPEG)
	if (!prefer_hardware)
		return make_ffmpeg_decoder(false);
#endif
	(void)prefer_hardware;
	return make_videotoolbox_decoder();
#elif defined(LUMACAM_HAVE_FFMPEG)
	return make_ffmpeg_decoder(prefer_hardware);
#else
	(void)prefer_hardware;
	return nullptr;
#endif
}
