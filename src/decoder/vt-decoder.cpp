/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Hardware HEVC / H.264 decoding on macOS with VideoToolbox.

#ifdef __APPLE__

#include "video-decoder.hpp"

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <vector>

namespace {

struct DecodeContext {
	const FrameSink *sink;
	bool full_range;
	bool bt709;
	bool ok;
	bool produced;
};

class VTDecoder final : public VideoDecoder {
public:
	~VTDecoder() override { reset(); }

	bool configure(const lcp::VideoConfig &config, std::string &error) override
	{
		auto units = lcp::split_annexb(config.parameter_sets.data(), config.parameter_sets.size());
		std::vector<const uint8_t *> pointers;
		std::vector<size_t> sizes;
		for (const auto &unit : units) {
			const uint8_t type = lcp::nal_type(config.codec, unit);
			const bool is_param = config.codec == lcp::Codec::HEVC ? (type >= 32 && type <= 34)
									       : (type == 7 || type == 8);
			if (is_param) {
				pointers.push_back(unit.data);
				sizes.push_back(unit.size);
			}
		}
		if (pointers.empty()) {
			error = "VIDEO_CONFIG has no parameter sets";
			return false;
		}

		CMVideoFormatDescriptionRef format = nullptr;
		OSStatus status;
		if (config.codec == lcp::Codec::HEVC) {
			status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
										     pointers.size(), pointers.data(),
										     sizes.data(), 4, nullptr, &format);
		} else {
			status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
				kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), 4, &format);
		}
		if (status != noErr || !format) {
			error = "invalid parameter sets (" + std::to_string((int)status) + ")";
			return false;
		}

		if (session_ && VTDecompressionSessionCanAcceptFormatDescription(session_, format)) {
			CFRelease(format_);
			format_ = format;
		} else {
			reset();
			format_ = format;
			if (!create_session(error)) {
				reset();
				return false;
			}
		}
		codec_ = config.codec;
		full_range_ = config.full_range;
		bt709_ = config.colorspace != 0;
		return true;
	}

	bool decode(const uint8_t *data, size_t size, bool, uint64_t pts_us, const FrameSink &sink) override
	{
		if (!session_ || !format_)
			return false;

		// Annex B -> 4-byte length prefixes, dropping in-band parameter sets / delimiters.
		buffer_.clear();
		for (const auto &unit : lcp::split_annexb(data, size)) {
			if (unit.size == 0 || lcp::is_out_of_band_nal(codec_, lcp::nal_type(codec_, unit)))
				continue;
			const uint32_t length = (uint32_t)unit.size;
			const uint8_t prefix[4] = {(uint8_t)(length >> 24), (uint8_t)(length >> 16),
						   (uint8_t)(length >> 8), (uint8_t)length};
			buffer_.insert(buffer_.end(), prefix, prefix + 4);
			buffer_.insert(buffer_.end(), unit.data, unit.data + unit.size);
		}
		if (buffer_.empty())
			return true;

		CMBlockBufferRef block = nullptr;
		OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, buffer_.size(),
								     kCFAllocatorDefault, nullptr, 0, buffer_.size(),
								     kCMBlockBufferAssureMemoryNowFlag, &block);
		if (status != kCMBlockBufferNoErr)
			return false;
		CMBlockBufferReplaceDataBytes(buffer_.data(), block, 0, buffer_.size());

		CMSampleBufferRef sample = nullptr;
		CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake((int64_t)pts_us, 1000000), kCMTimeInvalid};
		const size_t sample_size = buffer_.size();
		status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 1, &timing, 1, &sample_size,
						   &sample);
		CFRelease(block);
		if (status != noErr || !sample)
			return false;

		DecodeContext context = {&sink, full_range_, bt709_, true, false};
		VTDecodeInfoFlags info = 0;
		status = VTDecompressionSessionDecodeFrame(session_, sample, 0 /* synchronous */, &context, &info);
		CFRelease(sample);

		if (status == kVTInvalidSessionErr) {
			// Happens after sleep / GPU change: rebuild the session for the next keyframe.
			std::string ignored;
			VTDecompressionSessionInvalidate(session_);
			CFRelease(session_);
			session_ = nullptr;
			create_session(ignored);
			return false;
		}
		return status == noErr && context.ok;
	}

	std::string description() const override { return "VideoToolbox (hardware)"; }

private:
	bool create_session(std::string &error)
	{
		const int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
		CFNumberRef format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
		const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
		const void *values[] = {format_number};
		CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
								&kCFTypeDictionaryKeyCallBacks,
								&kCFTypeDictionaryValueCallBacks);
		CFRelease(format_number);

		const void *spec_keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
		const void *spec_values[] = {kCFBooleanTrue};
		CFDictionaryRef spec = CFDictionaryCreate(kCFAllocatorDefault, spec_keys, spec_values, 1,
							  &kCFTypeDictionaryKeyCallBacks,
							  &kCFTypeDictionaryValueCallBacks);

		VTDecompressionOutputCallbackRecord callback = {&VTDecoder::on_output, this};
		OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, format_, spec, attributes,
							       &callback, &session_);
		CFRelease(spec);
		CFRelease(attributes);
		if (status != noErr) {
			error = "VideoToolbox session failed (" + std::to_string((int)status) + ")";
			session_ = nullptr;
			return false;
		}
		VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
		return true;
	}

	static void on_output(void *, void *frame_refcon, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image,
			      CMTime, CMTime)
	{
		auto *context = static_cast<DecodeContext *>(frame_refcon);
		if (!context)
			return;
		if (status != noErr || !image) {
			context->ok = false;
			return;
		}
		if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
			context->ok = false;
			return;
		}

		obs_source_frame frame = {};
		frame.format = VIDEO_FORMAT_NV12;
		frame.width = (uint32_t)CVPixelBufferGetWidth(image);
		frame.height = (uint32_t)CVPixelBufferGetHeight(image);
		for (size_t plane = 0; plane < 2; plane++) {
			frame.data[plane] = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, plane);
			frame.linesize[plane] = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, plane);
		}
		set_frame_color(frame, context->bt709, context->full_range);
		(*context->sink)(frame);
		context->produced = true;

		CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	}

	void reset()
	{
		if (session_) {
			VTDecompressionSessionWaitForAsynchronousFrames(session_);
			VTDecompressionSessionInvalidate(session_);
			CFRelease(session_);
			session_ = nullptr;
		}
		if (format_) {
			CFRelease(format_);
			format_ = nullptr;
		}
	}

	VTDecompressionSessionRef session_ = nullptr;
	CMVideoFormatDescriptionRef format_ = nullptr;
	lcp::Codec codec_ = lcp::Codec::HEVC;
	bool full_range_ = false;
	bool bt709_ = true;
	std::vector<uint8_t> buffer_;
};

} // namespace

std::unique_ptr<VideoDecoder> make_videotoolbox_decoder()
{
	return std::make_unique<VTDecoder>();
}

#endif // __APPLE__
