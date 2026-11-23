/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lumacam-source.hpp"

#include "net/discovery.hpp"
#include "net/usbmux.hpp"

#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>

#define T(key) obs_module_text(key)

namespace {

using clock_type = std::chrono::steady_clock;

constexpr int kIdleTimeoutMs = 6000;
constexpr auto kPingInterval = std::chrono::seconds(2);

/* Owning wrapper for obs_data_t. */
struct Data {
	obs_data_t *ptr;
	explicit Data(obs_data_t *p = obs_data_create()) : ptr(p) {}
	~Data()
	{
		if (ptr)
			obs_data_release(ptr);
	}
	Data(const Data &) = delete;
	Data &operator=(const Data &) = delete;
	operator obs_data_t *() const { return ptr; }
	std::string json() const
	{
		const char *text = ptr ? obs_data_get_json(ptr) : nullptr;
		return text ? text : "{}";
	}
};

void parse_resolution(const char *value, int &width, int &height)
{
	width = 1920;
	height = 1080;
	if (value)
		sscanf(value, "%dx%d", &width, &height);
}

obs_data_t *build_video(obs_data_t *settings)
{
	int width, height;
	parse_resolution(obs_data_get_string(settings, "resolution"), width, height);
	obs_data_t *video = obs_data_create();
	obs_data_set_string(video, "codec", obs_data_get_string(settings, "codec"));
	obs_data_set_int(video, "width", width);
	obs_data_set_int(video, "height", height);
	obs_data_set_int(video, "fps", obs_data_get_int(settings, "fps"));
	obs_data_set_int(video, "bitrate", obs_data_get_int(settings, "bitrate") * 1000000);
	obs_data_set_string(video, "orientation", obs_data_get_string(settings, "orientation"));
	obs_data_set_string(video, "stabilization", obs_data_get_string(settings, "stabilization"));
	return video;
}

std::string build_video_message(obs_data_t *settings, const char *cmd)
{
	Data message;
	if (cmd)
		obs_data_set_string(message, "cmd", cmd);
	else
		obs_data_set_int(message, "protocol", lcp::kVersion);
	obs_data_t *video = build_video(settings);
	obs_data_set_obj(message, "video", video);
	obs_data_release(video);
	obs_data_set_string(message, "camera", obs_data_get_string(settings, "camera"));
	return message.json();
}

std::string build_controls_message(obs_data_t *settings)
{
	Data set;
	obs_data_set_double(set, "zoom", obs_data_get_double(settings, "zoom"));
	obs_data_set_string(set, "focusMode", obs_data_get_string(settings, "focus_mode"));
	obs_data_set_double(set, "lensPosition", obs_data_get_double(settings, "lens_position"));
	obs_data_set_string(set, "exposureMode", obs_data_get_string(settings, "exposure_mode"));
	obs_data_set_int(set, "iso", obs_data_get_int(settings, "iso"));
	obs_data_set_int(set, "shutter", obs_data_get_int(settings, "shutter"));
	obs_data_set_double(set, "exposureBias", obs_data_get_double(settings, "exposure_bias"));
	obs_data_set_string(set, "whiteBalanceMode", obs_data_get_string(settings, "wb_mode"));
	obs_data_set_int(set, "temperature", obs_data_get_int(settings, "temperature"));
	obs_data_set_int(set, "tint", obs_data_get_int(settings, "tint"));
	obs_data_set_bool(set, "torch", obs_data_get_bool(settings, "torch"));
	// "phone" = keep what was chosen on the iPhone; anything else is pushed to the phone,
	// which echoes it back in STATUS (the phone stays the single source of truth).
	const char *style = obs_data_get_string(settings, "style");
	if (strcmp(style, "phone") != 0)
		obs_data_set_string(set, "style", style);
	const char *look = obs_data_get_string(settings, "look");
	if (strcmp(look, "phone") != 0)
		obs_data_set_string(set, "look", look);
	Data message;
	obs_data_set_obj(message, "set", set);
	return message.json();
}

/* Adds the OBS client name and the pairing code to HELLO. */
std::string with_client(const std::string &hello_json, const std::string &code)
{
	Data hello(obs_data_create_from_json(hello_json.c_str()));
	if (!hello.ptr)
		return hello_json;
	obs_data_set_string(hello, "client", (std::string("lumacam-obs/") + PLUGIN_VERSION).c_str());
	if (!code.empty())
		obs_data_set_string(hello, "code", code.c_str());
	return hello.json();
}

constexpr const char *kLookFilterName = "LumaCam Look";
constexpr const char *kLooks[] = {"natural", "vivid", "warm", "cool", "cinema", "mono"};

bool is_known_look(const std::string &look)
{
	for (const char *known : kLooks)
		if (look == known)
			return true;
	return false;
}

struct LookTask {
	obs_weak_source_t *source;
	std::string look;
};

/* Runs on the OBS UI thread: adds, updates or disables the source's "Apply LUT" filter. The
   filter keeps a fixed name so it is found again after OBS restarts (filters are saved). */
void apply_look_task(void *param)
{
	std::unique_ptr<LookTask> task(static_cast<LookTask *>(param));
	obs_source_t *source = obs_weak_source_get_source(task->source);
	obs_weak_source_release(task->source);
	if (!source)
		return;

	obs_source_t *filter = obs_source_get_filter_by_name(source, kLookFilterName);
	if (task->look == "natural") {
		if (filter)
			obs_source_set_enabled(filter, false);
	} else {
		char *path = obs_module_file(("luts/" + task->look + ".cube").c_str());
		if (path) {
			Data settings;
			obs_data_set_string(settings, "image_path", path);
			obs_data_set_double(settings, "clut_amount", 1.0);
			if (filter) {
				obs_source_update(filter, settings);
				obs_source_set_enabled(filter, true);
			} else {
				filter = obs_source_create("clut_filter", kLookFilterName, settings, nullptr);
				if (filter)
					obs_source_filter_add(source, filter);
			}
			bfree(path);
		} else {
			obs_log(LOG_WARNING, "missing LUT for look %s", task->look.c_str());
		}
	}
	obs_source_release(filter);
	obs_source_release(source);
}

const char *route_label(connector::Route route)
{
	switch (route) {
	case connector::Route::Usb:
		return T("Route.USB");
	case connector::Route::Hotspot:
		return T("Route.Hotspot");
	case connector::Route::Wifi:
		return T("Route.WiFi");
	case connector::Route::Manual:
		return T("Route.Manual");
	}
	return "";
}

bool set_visible(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (p)
		obs_property_set_visible(p, visible);
	return true;
}

bool on_device_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = strcmp(obs_data_get_string(settings, "device"), "manual") == 0;
	set_visible(props, "host", manual);
	set_visible(props, "port", manual);
	return true;
}

bool on_focus_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	return set_visible(props, "lens_position", strcmp(obs_data_get_string(settings, "focus_mode"), "manual") == 0);
}

bool on_exposure_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = strcmp(obs_data_get_string(settings, "exposure_mode"), "manual") == 0;
	set_visible(props, "iso", manual);
	set_visible(props, "shutter", manual);
	set_visible(props, "exposure_bias", !manual);
	return true;
}

bool on_wb_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = strcmp(obs_data_get_string(settings, "wb_mode"), "manual") == 0;
	set_visible(props, "temperature", manual);
	set_visible(props, "tint", manual);
	return true;
}

bool on_refresh(obs_properties_t *, obs_property_t *, void *)
{
	return true; // rebuilds the properties: new device list and status
}

std::string short_serial(const std::string &serial)
{
	return serial.size() > 8 ? serial.substr(serial.size() - 8) : serial;
}

} // namespace

/* ---------------------------------------------------------------------- */

LumaCamSource::LumaCamSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
	start_worker();
}

LumaCamSource::~LumaCamSource()
{
	stop_worker();
}

void LumaCamSource::defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "code", "");
	obs_data_set_default_string(settings, "style", "phone");
	obs_data_set_default_string(settings, "look", "phone");
	obs_data_set_default_string(settings, "device", "auto");
	obs_data_set_default_string(settings, "host", "");
	obs_data_set_default_int(settings, "port", lcp::kDefaultPort);

	obs_data_set_default_string(settings, "resolution", "1920x1080");
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_string(settings, "codec", "hevc");
	obs_data_set_default_int(settings, "bitrate", 25);
	obs_data_set_default_string(settings, "camera", "back-wide");
	obs_data_set_default_string(settings, "orientation", "landscape");
	obs_data_set_default_string(settings, "stabilization", "off");

	obs_data_set_default_double(settings, "zoom", 1.0);
	obs_data_set_default_string(settings, "focus_mode", "auto");
	obs_data_set_default_double(settings, "lens_position", 0.5);
	obs_data_set_default_string(settings, "exposure_mode", "auto");
	obs_data_set_default_int(settings, "iso", 400);
	obs_data_set_default_int(settings, "shutter", 60);
	obs_data_set_default_double(settings, "exposure_bias", 0.0);
	obs_data_set_default_string(settings, "wb_mode", "auto");
	obs_data_set_default_int(settings, "temperature", 5600);
	obs_data_set_default_int(settings, "tint", 0);
	obs_data_set_default_bool(settings, "torch", false);

	obs_data_set_default_bool(settings, "low_latency", true);
	obs_data_set_default_bool(settings, "hw_decode", true);
	obs_data_set_default_bool(settings, "clear_on_disconnect", false);
	obs_data_set_default_bool(settings, "deactivate_when_hidden", false);
}

void LumaCamSource::update(obs_data_t *settings)
{
	Connection connection;
	connection.device = obs_data_get_string(settings, "device");
	connection.host = obs_data_get_string(settings, "host");
	connection.port = (int)obs_data_get_int(settings, "port");
	connection.code = lcp::normalize_code(obs_data_get_string(settings, "code"));

	const std::string video = build_video_message(settings, nullptr);
	const std::string controls = build_controls_message(settings);

	const bool low_latency = obs_data_get_bool(settings, "low_latency");
	low_latency_ = low_latency;
	obs_source_set_async_unbuffered(source_, low_latency);
	clear_on_disconnect_ = obs_data_get_bool(settings, "clear_on_disconnect");
	deactivate_when_hidden_ = obs_data_get_bool(settings, "deactivate_when_hidden");
	const bool hw = obs_data_get_bool(settings, "hw_decode");
	const bool hw_changed = hw != hardware_decode_.exchange(hw);

	bool reconnect = false, video_changed = false, controls_changed = false;
	{
		std::lock_guard<std::mutex> lock(settings_mutex_);
		reconnect = connection != connection_ || hw_changed;
		video_changed = video != video_json_;
		controls_changed = controls != controls_json_;
		connection_ = connection;
		video_json_ = video;
		controls_json_ = controls;
	}

	if (!running_)
		return;
	if (reconnect) {
		stop_worker();
		start_worker();
		return;
	}
	if (video_changed)
		send_message(lcp::MessageType::Control, build_video_message(settings, "configure"));
	if (controls_changed)
		send_message(lcp::MessageType::Control, controls);
}

void LumaCamSource::show()
{
	if (deactivate_when_hidden_ && !running_)
		start_worker();
}

void LumaCamSource::hide()
{
	if (deactivate_when_hidden_ && running_) {
		stop_worker();
		set_message(T("Status.Paused"));
	}
}

/* Worker ------------------------------------------------------------------ */

void LumaCamSource::start_worker()
{
	if (running_)
		return;
	stop_ = false;
	running_ = true;
	worker_ = std::thread(&LumaCamSource::worker_main, this);
}

void LumaCamSource::stop_worker()
{
	if (!running_)
		return;
	stop_ = true;
	wake_cv_.notify_all();
	{
		std::lock_guard<std::mutex> lock(socket_mutex_);
		if (socket_)
			socket_->shutdown();
	}
	if (worker_.joinable())
		worker_.join();
	running_ = false;
	decoder_.reset();
}

void LumaCamSource::worker_main()
{
	os_set_thread_name("lumacam-receiver");
	set_message(T("Status.Searching"));

	while (!stop_) {
		std::string error;
		retry_delay_ms_ = 1500;
		const bool had_video = run_session(error);
		if (stop_)
			break;

		{
			std::lock_guard<std::mutex> lock(status_mutex_);
			status_.connected = false;
			status_.received_fps = 0;
		}
		set_message(error.empty() ? T("Status.Disconnected") : error);
		if (clear_on_disconnect_)
			obs_source_output_video(source_, nullptr);

		std::unique_lock<std::mutex> lock(wake_mutex_);
		wake_cv_.wait_for(lock, std::chrono::milliseconds(had_video ? 500 : retry_delay_ms_),
				  [this] { return stop_.load(); });
	}
}

std::string LumaCamSource::failure_message(connector::Failure failure, const Connection &connection) const
{
	switch (failure) {
	case connector::Failure::CodeNeeded:
		return connection.code.empty() ? T("Error.CodeNeeded") : T("Error.CodeFormat");
	case connector::Failure::NoMatch:
		return T("Error.NoMatch");
	case connector::Failure::UsbNotFound:
		return T("Error.UsbNotFound");
	case connector::Failure::ManualFailed:
		return connection.host.empty() ? T("Error.NoHost") : T("Error.ManualFailed");
	case connector::Failure::NoPhone:
		return connection.device.rfind("wifi:", 0) == 0 ? T("Error.WifiNotFound") : T("Error.NoPhone");
	default:
		return {};
	}
}

/* ERROR from the phone: pairing refusals end the session and set how long to wait. */
bool LumaCamSource::handle_phone_error(const std::string &json, std::string &error)
{
	Data data(obs_data_create_from_json(json.c_str()));
	const std::string code = data.ptr ? obs_data_get_string(data, "code") : "";
	obs_log(LOG_WARNING, "phone error %s: %s", code.c_str(), data.ptr ? obs_data_get_string(data, "message") : "");

	if (code == "replaced") {
		// Another computer (or another OBS source) took the phone: do not steal it right back,
		// or the two would keep taking turns.
		error = T("Error.Replaced");
		retry_delay_ms_ = 10000;
		return true;
	}
	if (code == "code_required" || code == "bad_code") {
		// Retrying will not help until the user types the right code (which restarts us).
		remembered_host_.clear();
		error = code == "bad_code" ? T("Error.BadCode") : T("Error.CodeNeeded");
		retry_delay_ms_ = 10000;
		return true;
	}
	if (code == "too_many_attempts") {
		const int seconds = data.ptr ? (int)obs_data_get_int(data, "retryAfter") : 30;
		char text[256];
		snprintf(text, sizeof(text), T("Error.TooManyAttempts"), seconds);
		error = text;
		retry_delay_ms_ = (seconds > 0 ? seconds : 30) * 1000;
		return true;
	}
	return false;
}

bool LumaCamSource::send_message(lcp::MessageType type, const std::string &payload)
{
	const auto packet = lcp::build_packet(type, 0, (uint64_t)(os_gettime_ns() / 1000), payload.data(),
					      payload.size());
	std::lock_guard<std::mutex> lock(socket_mutex_);
	if (!socket_)
		return false;
	std::string error;
	return socket_->send_all(packet.data(), packet.size(), error);
}

bool LumaCamSource::run_session(std::string &error)
{
	Connection connection;
	std::string hello, controls;
	{
		std::lock_guard<std::mutex> lock(settings_mutex_);
		connection = connection_;
		hello = with_client(video_json_, connection.code);
		controls = controls_json_;
	}

	connector::Request request;
	request.mode = connection.device;
	request.host = connection.host;
	request.port = (uint16_t)(connection.port > 0 ? connection.port : lcp::kDefaultPort);
	request.code = connection.code;
	request.remembered_host = remembered_host_;

	net::Socket socket;
	connector::Result route;
	connector::Failure failure = connector::Failure::None;
	std::string detail;
	if (!connector::connect(request, socket, route, failure, detail, stop_)) {
		error = failure_message(failure, connection);
		if (failure == connector::Failure::CodeNeeded || failure == connector::Failure::NoMatch)
			retry_delay_ms_ = 2500;
		return false;
	}
	if (route.route != connector::Route::Usb)
		remembered_host_ = route.host;

	{
		std::lock_guard<std::mutex> lock(socket_mutex_);
		socket_ = &socket;
	}
	struct SocketGuard {
		LumaCamSource *self;
		~SocketGuard()
		{
			std::lock_guard<std::mutex> lock(self->socket_mutex_);
			self->socket_ = nullptr;
		}
	} guard{this};

	if (stop_)
		return false;

	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		status_.connected = true;
		status_.route = route.route;
		status_.host = route.host;
		status_.message.clear();
	}
	obs_log(LOG_INFO, "connected to phone via %s%s%s", connector::route_name(route.route),
		route.host.empty() ? "" : " ", route.host.c_str());

	send_message(lcp::MessageType::Hello, hello);
	send_message(lcp::MessageType::Control, controls);

	decoder_.reset();
	bool have_config = false;
	bool waiting_for_keyframe = true;
	bool had_video = false;
	uint64_t current_pts = 0;
	uint64_t pts_offset = 0;
	bool pts_offset_valid = false;
	int frames_this_second = 0;
	auto stats_start = clock_type::now();
	auto last_ping = clock_type::now();
	auto last_data = clock_type::now();

	const FrameSink sink = [&](obs_source_frame &frame) {
		if (low_latency_) {
			frame.timestamp = os_gettime_ns();
		} else {
			const uint64_t pts_ns = current_pts * 1000;
			if (!pts_offset_valid) {
				pts_offset = os_gettime_ns() - pts_ns;
				pts_offset_valid = true;
			}
			frame.timestamp = pts_ns + pts_offset;
		}
		obs_source_output_video(source_, &frame);
		frames_this_second++;
	};

	uint8_t header_bytes[lcp::kHeaderSize];
	std::vector<uint8_t> payload;

	while (!stop_) {
		const int ready = socket.wait_readable(250);
		const auto now = clock_type::now();

		if (now - last_ping >= kPingInterval) {
			send_message(lcp::MessageType::Ping, "");
			last_ping = now;
		}
		if (now - stats_start >= std::chrono::seconds(1)) {
			const double seconds = std::chrono::duration<double>(now - stats_start).count();
			std::lock_guard<std::mutex> lock(status_mutex_);
			status_.received_fps = frames_this_second / seconds;
			frames_this_second = 0;
			stats_start = now;
		}
		if (ready < 0) {
			error = T("Error.ConnectionLost");
			return had_video;
		}
		if (ready == 0) {
			if (now - last_data > std::chrono::milliseconds(kIdleTimeoutMs)) {
				error = T("Error.Timeout");
				return had_video;
			}
			continue;
		}

		if (!socket.recv_exact(header_bytes, sizeof(header_bytes), stop_, kIdleTimeoutMs, error))
			return had_video;
		lcp::Header header;
		if (!lcp::decode_header(header_bytes, header, error))
			return had_video;
		payload.resize(header.length);
		if (header.length && !socket.recv_exact(payload.data(), payload.size(), stop_, kIdleTimeoutMs, error))
			return had_video;
		last_data = clock_type::now();

		switch ((lcp::MessageType)header.type) {
		case lcp::MessageType::ServerInfo:
			handle_server_info(std::string(payload.begin(), payload.end()));
			break;

		case lcp::MessageType::VideoConfig: {
			lcp::VideoConfig config;
			if (!lcp::parse_video_config(payload.data(), payload.size(), config)) {
				obs_log(LOG_WARNING, "invalid VIDEO_CONFIG");
				break;
			}
			if (!decoder_)
				decoder_ = VideoDecoder::create(hardware_decode_);
			std::string decoder_error;
			if (!decoder_ || !decoder_->configure(config, decoder_error)) {
				obs_log(LOG_ERROR, "decoder configuration failed: %s", decoder_error.c_str());
				set_message(std::string(T("Error.Decoder")) + " " + decoder_error);
				have_config = false;
				break;
			}
			have_config = true;
			waiting_for_keyframe = true;
			std::lock_guard<std::mutex> lock(status_mutex_);
			status_.width = config.width;
			status_.height = config.height;
			status_.codec = config.codec == lcp::Codec::HEVC ? "HEVC" : "H.264";
			status_.decoder = decoder_->description();
			break;
		}

		case lcp::MessageType::VideoFrame: {
			if (!have_config || !decoder_)
				break;
			const bool keyframe = (header.flags & lcp::kFlagKeyframe) != 0;
			if (waiting_for_keyframe && !keyframe)
				break;
			current_pts = header.timestamp;
			if (decoder_->decode(payload.data(), payload.size(), keyframe, header.timestamp, sink)) {
				waiting_for_keyframe = false;
				had_video = true;
			} else {
				waiting_for_keyframe = true;
				send_message(lcp::MessageType::Control, "{\"cmd\":\"keyframe\"}");
			}
			break;
		}

		case lcp::MessageType::Status:
			handle_status(std::string(payload.begin(), payload.end()));
			{
				std::string look;
				{
					std::lock_guard<std::mutex> lock(status_mutex_);
					look = status_.look;
				}
				apply_look(look);
			}
			break;

		case lcp::MessageType::Ping: {
			const auto pong = lcp::build_packet(lcp::MessageType::Pong, 0, header.timestamp, nullptr, 0);
			std::lock_guard<std::mutex> lock(socket_mutex_);
			std::string ignored;
			socket.send_all(pong.data(), pong.size(), ignored);
			break;
		}

		case lcp::MessageType::Error:
			if (handle_phone_error(std::string(payload.begin(), payload.end()), error))
				return had_video;
			break;

		default:
			break; // unknown / client-to-server types are ignored
		}
	}
	return had_video;
}

void LumaCamSource::handle_server_info(const std::string &json)
{
	Data info(obs_data_create_from_json(json.c_str()));
	if (!info.ptr)
		return;
	std::vector<CameraInfo> cameras;
	obs_data_array_t *array = obs_data_get_array(info, "cameras");
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		cameras.push_back({obs_data_get_string(item, "id"), obs_data_get_string(item, "name")});
		obs_data_release(item);
	}
	obs_data_array_release(array);

	std::lock_guard<std::mutex> lock(status_mutex_);
	status_.device = obs_data_get_string(info, "device");
	status_.model = obs_data_get_string(info, "model");
	status_.app = obs_data_get_string(info, "app");
	status_.cameras = cameras;
	obs_log(LOG_INFO, "phone: %s (%s, %s)", status_.device.c_str(), status_.model.c_str(), status_.app.c_str());
}

void LumaCamSource::handle_status(const std::string &json)
{
	Data data(obs_data_create_from_json(json.c_str()));
	if (!data.ptr)
		return;
	std::lock_guard<std::mutex> lock(status_mutex_);
	status_.phone_fps = obs_data_get_double(data, "fps");
	status_.bitrate = obs_data_get_double(data, "bitrate");
	status_.battery = obs_data_get_double(data, "battery");
	status_.charging = obs_data_get_bool(data, "charging");
	status_.thermal = obs_data_get_string(data, "thermal");
	status_.throttled = obs_data_get_bool(data, "throttled");
	status_.style = obs_data_get_string(data, "style");
	status_.look = obs_data_get_string(data, "look");
}

/* Worker thread. The LUT filter itself is changed on the UI thread. */
void LumaCamSource::apply_look(const std::string &look)
{
	if (look == applied_look_ || !is_known_look(look))
		return;
	applied_look_ = look;
	obs_log(LOG_INFO, "color look: %s", look.c_str());
	obs_queue_task(OBS_TASK_UI, apply_look_task, new LookTask{obs_source_get_weak_source(source_), look}, false);
}

void LumaCamSource::set_message(const std::string &message)
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	status_.message = message;
}

std::string LumaCamSource::status_text()
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	const PhoneStatus &s = status_;
	char line[512];

	if (!s.connected) {
		std::string text = s.message.empty() ? T("Status.Searching") : s.message;
		text += "\n";
		text += T("Status.Hint");
		return text;
	}

	std::string text;
	const std::string via = s.host.empty() || s.route == connector::Route::Hotspot
					? route_label(s.route)
					: std::string(route_label(s.route)) + " · " + s.host;
	snprintf(line, sizeof(line), "%s %s (%s)\n", T("Status.ConnectedTo"),
		 s.device.empty() ? "LumaCam" : s.device.c_str(), via.c_str());
	text += line;
	if (s.width > 0) {
		snprintf(line, sizeof(line), "%d×%d · %.1f fps · %s · %.1f Mbps\n", s.width, s.height,
			 s.received_fps, s.codec.c_str(), s.bitrate / 1e6);
		text += line;
	}
	if (s.battery >= 0) {
		snprintf(line, sizeof(line), "%s %d%%%s · %s %s", T("Status.Battery"), (int)std::lround(s.battery * 100),
			 s.charging ? T("Status.Charging") : "", T("Status.Thermal"), s.thermal.c_str());
		text += line;
		if (s.throttled) {
			text += " · ";
			text += T("Status.Throttled");
		}
		text += "\n";
	}
	if (!s.decoder.empty()) {
		text += T("Status.Decoder");
		text += " " + s.decoder;
	}
	return text;
}

/* Properties ---------------------------------------------------------------- */

obs_properties_t *LumaCamSource::properties()
{
	obs_properties_t *props = obs_properties_create();

	bool connected;
	std::vector<CameraInfo> cameras;
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		connected = status_.connected;
		cameras = status_.cameras;
	}

	obs_property_t *info = obs_properties_add_text(props, "status_info", status_text().c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_type(info, connected ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_WARNING);
	obs_properties_add_button2(props, "refresh", T("Refresh"), on_refresh, this);

	obs_property_t *code = obs_properties_add_text(props, "code", T("Code"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(code, T("Code.Description"));

	/* Connection */
	obs_properties_t *conn = obs_properties_create();
	obs_property_t *device =
		obs_properties_add_list(conn, "device", T("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(device, T("Device.Auto"), "auto");

	std::vector<usbmux::Device> usb_devices;
	std::string usb_error;
	if (usbmux::list_devices(usb_devices, usb_error)) {
		for (const auto &d : usb_devices) {
			const std::string label = std::string("iPhone / iPad (USB) — ") + short_serial(d.serial);
			obs_property_list_add_string(device, label.c_str(), ("usb:" + d.serial).c_str());
		}
	}
	for (const auto &service : discovery::services()) {
		const std::string label = service.name + " (Wi-Fi)";
		obs_property_list_add_string(device, label.c_str(), ("wifi:" + service.name).c_str());
	}
	obs_property_list_add_string(device, T("Device.Network"), "network");
	obs_property_list_add_string(device, T("Device.Manual"), "manual");
	{
		std::lock_guard<std::mutex> lock(settings_mutex_);
		const std::string &current = connection_.device;
		bool found = false;
		const size_t count = obs_property_list_item_count(device);
		for (size_t i = 0; i < count; i++)
			found = found || current == obs_property_list_item_string(device, i);
		if (!found && !current.empty()) {
			const std::string label = current.substr(current.find(':') + 1) + " " + T("Device.Offline");
			obs_property_list_add_string(device, label.c_str(), current.c_str());
		}
	}
	obs_property_set_long_description(device, T("Device.Description"));
	obs_property_set_modified_callback(device, on_device_modified);
	obs_properties_add_text(conn, "host", T("Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(conn, "port", T("Port"), 1024, 65535, 1);
	obs_properties_add_group(props, "group_connection", T("Group.Connection"), OBS_GROUP_NORMAL, conn);
	obs_property_set_long_description(obs_properties_get(props, "group_connection"), T("Group.Connection.Description"));

	/* Video */
	obs_properties_t *video = obs_properties_create();
	obs_property_t *resolution = obs_properties_add_list(video, "resolution", T("Resolution"), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(resolution, "3840×2160 (4K UHD)", "3840x2160");
	obs_property_list_add_string(resolution, "2560×1440 (QHD)", "2560x1440");
	obs_property_list_add_string(resolution, "1920×1080 (Full HD)", "1920x1080");
	obs_property_list_add_string(resolution, "1280×720 (HD)", "1280x720");

	obs_property_t *fps =
		obs_properties_add_list(video, "fps", T("FrameRate"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int value : {24, 25, 30, 50, 60})
		obs_property_list_add_int(fps, std::to_string(value).c_str(), value);

	obs_property_t *codec =
		obs_properties_add_list(video, "codec", T("Codec"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec, T("Codec.HEVC"), "hevc");
	obs_property_list_add_string(codec, T("Codec.H264"), "h264");

	obs_property_t *bitrate = obs_properties_add_int_slider(video, "bitrate", T("Bitrate"), 2, 120, 1);
	obs_property_int_set_suffix(bitrate, " Mbps");
	obs_property_set_long_description(bitrate, T("Bitrate.Description"));

	obs_property_t *camera =
		obs_properties_add_list(video, "camera", T("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (cameras.empty()) {
		obs_property_list_add_string(camera, T("Camera.BackWide"), "back-wide");
		obs_property_list_add_string(camera, T("Camera.BackUltraWide"), "back-ultrawide");
		obs_property_list_add_string(camera, T("Camera.BackTele"), "back-tele");
		obs_property_list_add_string(camera, T("Camera.BackAuto"), "back-auto");
		obs_property_list_add_string(camera, T("Camera.Front"), "front");
	} else {
		for (const auto &c : cameras)
			obs_property_list_add_string(camera, c.name.c_str(), c.id.c_str());
	}

	obs_property_t *orientation = obs_properties_add_list(video, "orientation", T("Orientation"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(orientation, T("Orientation.Landscape"), "landscape");
	obs_property_list_add_string(orientation, T("Orientation.LandscapeFlipped"), "landscape-flipped");
	obs_property_list_add_string(orientation, T("Orientation.Portrait"), "portrait");
	obs_property_list_add_string(orientation, T("Orientation.PortraitFlipped"), "portrait-flipped");

	obs_property_t *stabilization = obs_properties_add_list(video, "stabilization", T("Stabilization"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(stabilization, T("Stabilization.Off"), "off");
	obs_property_list_add_string(stabilization, T("Stabilization.Standard"), "standard");
	obs_property_list_add_string(stabilization, T("Stabilization.Cinematic"), "cinematic");
	obs_property_set_long_description(stabilization, T("Stabilization.Description"));
	obs_properties_add_group(props, "group_video", T("Group.Video"), OBS_GROUP_NORMAL, video);

	/* Image: shooting style (on the phone) and color look (LUT here, on the GPU) */
	obs_properties_t *image = obs_properties_create();
	obs_property_t *style =
		obs_properties_add_list(image, "style", T("Style"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(style, T("FromPhone"), "phone");
	for (const char *value : {"natural", "cinematic", "low-light", "action"})
		obs_property_list_add_string(style, T((std::string("Style.") + value).c_str()), value);
	obs_property_set_long_description(style, T("Style.Description"));
	obs_property_t *look =
		obs_properties_add_list(image, "look", T("Look"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(look, T("FromPhone"), "phone");
	for (const char *value : kLooks)
		obs_property_list_add_string(look, T((std::string("Look.") + value).c_str()), value);
	obs_property_set_long_description(look, T("Look.Description"));
	obs_properties_add_group(props, "group_image", T("Group.Image"), OBS_GROUP_NORMAL, image);

	/* Camera controls */
	obs_properties_t *controls = obs_properties_create();
	obs_property_t *zoom = obs_properties_add_float_slider(controls, "zoom", T("Zoom"), 1.0, 15.0, 0.1);
	obs_property_float_set_suffix(zoom, "×");

	obs_property_t *focus = obs_properties_add_list(controls, "focus_mode", T("Focus"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(focus, T("Mode.Auto"), "auto");
	obs_property_list_add_string(focus, T("Mode.Manual"), "manual");
	obs_property_set_modified_callback(focus, on_focus_modified);
	obs_properties_add_float_slider(controls, "lens_position", T("LensPosition"), 0.0, 1.0, 0.01);

	obs_property_t *exposure = obs_properties_add_list(controls, "exposure_mode", T("Exposure"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(exposure, T("Mode.Auto"), "auto");
	obs_property_list_add_string(exposure, T("Mode.Manual"), "manual");
	obs_property_set_modified_callback(exposure, on_exposure_modified);
	obs_property_t *ev = obs_properties_add_float_slider(controls, "exposure_bias", T("ExposureBias"), -3.0, 3.0, 0.1);
	obs_property_float_set_suffix(ev, " EV");
	obs_properties_add_int_slider(controls, "iso", T("ISO"), 25, 6400, 1);
	obs_property_t *shutter = obs_properties_add_list(controls, "shutter", T("Shutter"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_INT);
	for (int value : {24, 25, 30, 48, 50, 60, 100, 120, 125, 250, 500, 1000, 2000, 4000, 8000})
		obs_property_list_add_int(shutter, ("1/" + std::to_string(value) + " s").c_str(), value);

	obs_property_t *wb = obs_properties_add_list(controls, "wb_mode", T("WhiteBalance"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(wb, T("Mode.Auto"), "auto");
	obs_property_list_add_string(wb, T("Mode.Manual"), "manual");
	obs_property_set_modified_callback(wb, on_wb_modified);
	obs_property_t *temperature =
		obs_properties_add_int_slider(controls, "temperature", T("Temperature"), 2000, 10000, 100);
	obs_property_int_set_suffix(temperature, " K");
	obs_properties_add_int_slider(controls, "tint", T("Tint"), -150, 150, 1);
	obs_properties_add_bool(controls, "torch", T("Torch"));
	obs_properties_add_group(props, "group_controls", T("Group.Controls"), OBS_GROUP_NORMAL, controls);

	/* Advanced */
	obs_properties_t *advanced = obs_properties_create();
	obs_property_t *latency = obs_properties_add_bool(advanced, "low_latency", T("LowLatency"));
	obs_property_set_long_description(latency, T("LowLatency.Description"));
	obs_properties_add_bool(advanced, "hw_decode", T("HardwareDecode"));
	obs_properties_add_bool(advanced, "clear_on_disconnect", T("ClearOnDisconnect"));
	obs_properties_add_bool(advanced, "deactivate_when_hidden", T("DeactivateWhenHidden"));
	obs_properties_add_group(props, "group_advanced", T("Group.Advanced"), OBS_GROUP_NORMAL, advanced);

	return props;
}

/* Registration ------------------------------------------------------------ */

void LumaCamSource::register_source()
{
	obs_source_info info = {};
	info.id = "lumacam_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = [](void *) { return T("LumaCam.Name"); };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new LumaCamSource(settings, source);
	};
	info.destroy = [](void *data) { delete static_cast<LumaCamSource *>(data); };
	info.update = [](void *data, obs_data_t *settings) { static_cast<LumaCamSource *>(data)->update(settings); };
	info.get_defaults = &LumaCamSource::defaults;
	info.get_properties = [](void *data) -> obs_properties_t * {
		return static_cast<LumaCamSource *>(data)->properties();
	};
	info.show = [](void *data) { static_cast<LumaCamSource *>(data)->show(); };
	info.hide = [](void *data) { static_cast<LumaCamSource *>(data)->hide(); };
	obs_register_source(&info);
}
