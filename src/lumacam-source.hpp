/*
LumaCam for OBS
Copyright (C) 2026 LumaCam contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "decoder/video-decoder.hpp"
#include "lcp/protocol.hpp"
#include "net/connector.hpp"
#include "net/socket.hpp"

#include <obs-module.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* The "LumaCam" video source: connects to the phone app, decodes the stream and pushes frames
   into OBS. Network and decoding run on a dedicated worker thread. */
class LumaCamSource {
public:
	LumaCamSource(obs_data_t *settings, obs_source_t *source);
	~LumaCamSource();

	void update(obs_data_t *settings);
	obs_properties_t *properties();
	void show();
	void hide();

	static void defaults(obs_data_t *settings);
	static void register_source();

private:
	struct Connection {
		std::string device; // "auto" | "manual" | "usb:<serial>" | "wifi:<service name>"
		std::string host;
		int port = lcp::kDefaultPort;
		std::string code; // 4-digit pairing code (digits only), needed off USB
		bool operator==(const Connection &o) const
		{
			return device == o.device && host == o.host && port == o.port && code == o.code;
		}
		bool operator!=(const Connection &o) const { return !(*this == o); }
	};

	struct CameraInfo {
		std::string id;
		std::string name;
	};

	struct PhoneStatus {
		bool connected = false;
		connector::Route route = connector::Route::Usb;
		std::string host;     // IP address when connected over the network
		std::string device;   // phone name
		std::string model;
		std::string app;
		std::string decoder;
		std::string codec;
		int width = 0;
		int height = 0;
		double phone_fps = 0;
		double received_fps = 0;
		double bitrate = 0;
		double battery = -1;
		bool charging = false;
		std::string thermal;
		std::string style; // shooting style chosen on the phone
		std::string look;  // color look, applied here with a LUT filter
		bool throttled = false;
		std::string message; // last error / state
		std::vector<CameraInfo> cameras;
	};

	void start_worker();
	void stop_worker();
	void worker_main();
	bool run_session(std::string &error);
	std::string failure_message(connector::Failure failure, const Connection &connection) const;
	bool handle_phone_error(const std::string &json, std::string &error);
	bool send_message(lcp::MessageType type, const std::string &payload);
	void handle_status(const std::string &json);
	void apply_look(const std::string &look);
	void handle_server_info(const std::string &json);
	void set_message(const std::string &message);
	std::string status_text();

	obs_source_t *source_;

	std::thread worker_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> running_{false};
	std::mutex wake_mutex_;
	std::condition_variable wake_cv_;
	int retry_delay_ms_ = 1500;      // worker thread only: wait before the next attempt
	std::string remembered_host_;    // worker thread only: last network address that worked
	std::string applied_look_;       // worker thread only: look the LUT filter is set to

	std::mutex socket_mutex_;
	net::Socket *socket_ = nullptr; // valid while a session is open

	std::mutex settings_mutex_;
	Connection connection_;
	std::string video_json_;    // {"video":{...},"camera":"..."} as sent in HELLO/configure
	std::string controls_json_; // {"set":{...}}
	std::atomic<bool> low_latency_{true};
	std::atomic<bool> hardware_decode_{true};
	std::atomic<bool> clear_on_disconnect_{false};
	std::atomic<bool> deactivate_when_hidden_{false};

	std::mutex status_mutex_;
	PhoneStatus status_;

	std::unique_ptr<VideoDecoder> decoder_; // worker thread only
};
