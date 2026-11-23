/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// HEVC / H.264 decoding with FFmpeg (libavcodec), with hardware acceleration when available:
// D3D11VA / DXVA2 / CUDA on Windows, VAAPI / CUDA on Linux, software fallback everywhere.

#ifdef LUMACAM_HAVE_FFMPEG

#include "video-decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <vector>

namespace {

video_format to_obs_format(int format)
{
	switch (format) {
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

class FFmpegDecoder final : public VideoDecoder {
public:
	explicit FFmpegDecoder(bool prefer_hardware) : prefer_hardware_(prefer_hardware) {}
	~FFmpegDecoder() override { close(); }

	bool configure(const lcp::VideoConfig &config, std::string &error) override
	{
		if (context_ && config.codec == codec_ && config.width == width_ && config.height == height_) {
			parameter_sets_ = config.parameter_sets;
			full_range_ = config.full_range;
			return true; // same stream shape: in-band parameter sets are enough
		}
		close();
		codec_ = config.codec;
		width_ = config.width;
		height_ = config.height;
		full_range_ = config.full_range;
		parameter_sets_ = config.parameter_sets;

		const AVCodec *codec =
			avcodec_find_decoder(config.codec == lcp::Codec::HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
		if (!codec) {
			error = "FFmpeg has no decoder for this codec";
			return false;
		}
		context_ = avcodec_alloc_context3(codec);
		packet_ = av_packet_alloc();
		frame_ = av_frame_alloc();
		sw_frame_ = av_frame_alloc();
		if (!context_ || !packet_ || !frame_ || !sw_frame_) {
			error = "out of memory";
			close();
			return false;
		}

		context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
		context_->thread_type = FF_THREAD_SLICE; // frame threading would add latency
		context_->thread_count = 0;
		context_->width = config.width;
		context_->height = config.height;

		context_->extradata = (uint8_t *)av_mallocz(parameter_sets_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(context_->extradata, parameter_sets_.data(), parameter_sets_.size());
		context_->extradata_size = (int)parameter_sets_.size();

		if (prefer_hardware_)
			setup_hardware(codec);

		if (avcodec_open2(context_, codec, nullptr) < 0) {
			error = "cannot open FFmpeg decoder";
			close();
			return false;
		}
		return true;
	}

	bool decode(const uint8_t *data, size_t size, bool keyframe, uint64_t pts_us, const FrameSink &sink) override
	{
		if (!context_)
			return false;

		const size_t extra = keyframe ? parameter_sets_.size() : 0;
		if (av_new_packet(packet_, (int)(size + extra)) < 0)
			return false;
		if (extra)
			memcpy(packet_->data, parameter_sets_.data(), extra);
		memcpy(packet_->data + extra, data, size);
		packet_->pts = (int64_t)pts_us;
		if (keyframe)
			packet_->flags |= AV_PKT_FLAG_KEY;

		int ret = avcodec_send_packet(context_, packet_);
		av_packet_unref(packet_);
		if (ret < 0 && ret != AVERROR(EAGAIN))
			return false;

		while ((ret = avcodec_receive_frame(context_, frame_)) >= 0) {
			AVFrame *output = frame_;
			if (hw_pix_fmt_ != AV_PIX_FMT_NONE && frame_->format == hw_pix_fmt_) {
				av_frame_unref(sw_frame_);
				if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
					av_frame_unref(frame_);
					return false;
				}
				sw_frame_->color_range = frame_->color_range;
				sw_frame_->colorspace = frame_->colorspace;
				output = sw_frame_;
			}
			emit(output, sink);
			av_frame_unref(frame_);
		}
		return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF;
	}

	std::string description() const override
	{
		if (hw_type_ != AV_HWDEVICE_TYPE_NONE)
			return std::string("FFmpeg ") + av_hwdevice_get_type_name(hw_type_) + " (hardware)";
		return "FFmpeg (software)";
	}

private:
	static enum AVPixelFormat get_format(AVCodecContext *context, const enum AVPixelFormat *formats)
	{
		auto *self = static_cast<FFmpegDecoder *>(context->opaque);
		for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == self->hw_pix_fmt_)
				return *p;
		}
		// Hardware path refused (unsupported profile / size): fall back to software.
		self->hw_pix_fmt_ = AV_PIX_FMT_NONE;
		self->hw_type_ = AV_HWDEVICE_TYPE_NONE;
		for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
			const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
			if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
				return *p;
		}
		return AV_PIX_FMT_NONE;
	}

	void setup_hardware(const AVCodec *codec)
	{
		static const AVHWDeviceType preferred[] = {
#ifdef _WIN32
			AV_HWDEVICE_TYPE_D3D11VA,
			AV_HWDEVICE_TYPE_DXVA2,
#else
			AV_HWDEVICE_TYPE_VAAPI,
#endif
			AV_HWDEVICE_TYPE_CUDA,
		};
		for (AVHWDeviceType type : preferred) {
			for (int i = 0;; i++) {
				const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
				if (!config)
					break;
				if (config->device_type != type ||
				    !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
					continue;
				if (av_hwdevice_ctx_create(&hw_device_, type, nullptr, nullptr, 0) < 0)
					break;
				context_->hw_device_ctx = av_buffer_ref(hw_device_);
				context_->opaque = this;
				context_->get_format = &FFmpegDecoder::get_format;
				hw_pix_fmt_ = config->pix_fmt;
				hw_type_ = type;
				return;
			}
		}
	}

	void emit(AVFrame *picture, const FrameSink &sink)
	{
		obs_source_frame frame = {};
		frame.format = to_obs_format(picture->format);
		if (frame.format == VIDEO_FORMAT_NONE)
			return;
		frame.width = (uint32_t)picture->width;
		frame.height = (uint32_t)picture->height;
		for (int i = 0; i < 3 && i < AV_NUM_DATA_POINTERS; i++) {
			frame.data[i] = picture->data[i];
			frame.linesize[i] = (uint32_t)picture->linesize[i];
		}
		const bool full_range = picture->color_range == AVCOL_RANGE_JPEG ||
					picture->format == AV_PIX_FMT_YUVJ420P ||
					(picture->color_range == AVCOL_RANGE_UNSPECIFIED && full_range_);
		const bool bt601 = picture->colorspace == AVCOL_SPC_BT470BG ||
				   picture->colorspace == AVCOL_SPC_SMPTE170M;
		set_frame_color(frame, !bt601, full_range);
		sink(frame);
	}

	void close()
	{
		if (context_)
			avcodec_free_context(&context_);
		if (packet_)
			av_packet_free(&packet_);
		if (frame_)
			av_frame_free(&frame_);
		if (sw_frame_)
			av_frame_free(&sw_frame_);
		if (hw_device_)
			av_buffer_unref(&hw_device_);
		hw_pix_fmt_ = AV_PIX_FMT_NONE;
		hw_type_ = AV_HWDEVICE_TYPE_NONE;
	}

	bool prefer_hardware_;
	AVCodecContext *context_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVFrame *sw_frame_ = nullptr;
	AVBufferRef *hw_device_ = nullptr;
	AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
	AVHWDeviceType hw_type_ = AV_HWDEVICE_TYPE_NONE;
	lcp::Codec codec_ = lcp::Codec::HEVC;
	uint16_t width_ = 0;
	uint16_t height_ = 0;
	bool full_range_ = false;
	std::vector<uint8_t> parameter_sets_;
};

} // namespace

std::unique_ptr<VideoDecoder> make_ffmpeg_decoder(bool prefer_hardware)
{
	return std::make_unique<FFmpegDecoder>(prefer_hardware);
}

#endif // LUMACAM_HAVE_FFMPEG
