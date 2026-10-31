/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * PipeWire implementation of AudioPassthru for Linux.
 *
 * Windows FxSound:  apps -> "FxSound Audio Enhancer" driver -> WASAPI loopback
 *                   capture -> DfxDsp -> WASAPI render -> real device
 *
 * Linux FxSound:    apps -> "FxSound Audio Enhancer" sink (a pw_stream with
 *                   media.class=Audio/Sink owned by this process) -> DfxDsp in
 *                   the stream's process callback -> ring buffer -> playback
 *                   pw_stream targeted at the real device
 *
 * Both streams share node.group (so one driver schedules them together) and
 * node.link-group (so the session manager never links our output back into
 * our own sink, which would create a feedback loop). This is the same scheme
 * PipeWire's own module-loopback uses.
 *
 * Threads:
 *  - PipeWire's thread loop runs registry/metadata events and stream state.
 *  - The PipeWire data thread runs the two process callbacks (real-time: no
 *    locks, no allocation, no syscalls).
 *  - A notifier thread delivers AudioPassthruCallback::onSoundDeviceChange,
 *    matching Windows where the callback comes from a COM notification thread.
 *    It is never called from a PipeWire thread, because the GUI's handler calls
 *    straight back into getSoundDevices().
 *  - The public methods are called from the GUI thread and lock the loop.
 */
#include "AudioPassthru.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/keys.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kChannels = 2;
constexpr int kFrameBytes = kChannels * (int)sizeof(float);
constexpr uint32_t kRingFrames = 1u << 15;   // ~680 ms at 48 kHz; power of two
constexpr uint32_t kMaxBlockFrames = 8192;   // DSP scratch size per process call
constexpr int kDefaultLatencyMs = 10;

std::wstring widen(const char* s)
{
	// UTF-8 -> wchar_t (UTF-32 on Linux)
	std::wstring out;
	const unsigned char* p = (const unsigned char*)(s ? s : "");
	while (*p) {
		uint32_t c = *p++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra) c &= (0x3F >> extra);
		for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i) c = (c << 6) | (*p++ & 0x3F);
		out += (wchar_t)c;
	}
	return out;
}

std::string narrow(const std::wstring& w)
{
	std::string out;
	for (wchar_t wc : w) {
		uint32_t c = (uint32_t)wc;
		if (c < 0x80) out += (char)c;
		else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
		else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
	}
	return out;
}

// Extract "name" from a default-sink metadata value such as {"name":"alsa_output.x"}.
std::string jsonName(const char* value)
{
	if (!value) return {};
	struct spa_json it[2];
	char key[64], name[1024];
	spa_json_init(&it[0], value, strlen(value));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0) return {};
	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (strcmp(key, "name") == 0) {
			if (spa_json_get_string(&it[1], name, sizeof(name)) > 0) return name;
			return {};
		}
		const char* v;
		if (spa_json_next(&it[1], &v) <= 0) break;
	}
	return {};
}

struct SinkNode {
	uint32_t id = SPA_ID_INVALID;
	std::string name;         // node.name
	std::string description;  // node.description
	std::string nick;         // node.nick
	std::string form_factor;  // device.form-factor
	int channels = 0;
	pw_proxy* proxy = nullptr;
	spa_hook listener {};
	class AudioPassthruPrivate* owner = nullptr;
};

} // namespace

class AudioPassthruPrivate
{
public:
	AudioPassthruPrivate();
	~AudioPassthruPrivate();

	int init();
	void mute(bool m) { mute_.store(m); }
	std::vector<SoundDevice> getSoundDevices(bool active_devices);
	int setBufferLength(int ms);
	int processTimer();
	void setDspProcessingModule(DfxDsp* dsp) { dsp_.store(dsp); }
	void setAsPlaybackDevice(const SoundDevice& dev);
	void registerCallback(AudioPassthruCallback* cb) { std::lock_guard<std::mutex> l(notify_mutex_); callback_ = cb; }
	bool isPlaybackDeviceAvailable();
	bool checkDeviceChanges() { return list_changed_.exchange(false); }
	void restoreDefaultPlaybackDevice();

private:
	// ---- PipeWire event handlers (thread-loop thread) ----
	static void onRegistryGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
	                             uint32_t version, const struct spa_dict* props);
	static void onRegistryGlobalRemove(void* data, uint32_t id);
	static void onNodeInfo(void* data, const struct pw_node_info* info);
	static int onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value);
	static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message);

	// ---- stream callbacks ----
	static void onCaptureProcess(void* data);
	static void onPlaybackProcess(void* data);
	static void onCaptureParamChanged(void* data, uint32_t id, const struct spa_pod* param);
	static void onPlaybackStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error);

	// Event tables, built in member functions so they can reference the
	// private static handlers above.
	static const pw_core_events* coreEvents();
	static const pw_registry_events* registryEvents();
	static const pw_node_events* nodeEvents();
	static const pw_metadata_events* metadataEvents();
	static const pw_stream_events* captureEvents();
	static const pw_stream_events* playbackEvents();

	// ---- helpers (call with loop locked) ----
	bool createCaptureStreamLocked();
	void connectPlaybackLocked();
	void destroyPlaybackLocked();
	void requestDefaultLocked(const std::string& node_name);
	bool isRealSinkLocked(const std::string& node_name) const;
	const SinkNode* findSinkLocked(const std::string& node_name) const;
	void deviceListChanged();   // marks change + schedules a notification
	void notifierThread();
	void waitSyncLocked();      // round-trip to the PipeWire server
	static void onCoreDone(void* data, uint32_t id, int seq);

	// ---- PipeWire objects ----
	pw_thread_loop* loop_ = nullptr;
	pw_context* context_ = nullptr;
	pw_core* core_ = nullptr;
	pw_registry* registry_ = nullptr;
	spa_hook core_listener_ {};
	spa_hook registry_listener_ {};
	pw_metadata* metadata_ = nullptr;
	uint32_t metadata_id_ = SPA_ID_INVALID;
	spa_hook metadata_listener_ {};
	pw_stream* capture_ = nullptr;
	pw_stream* playback_ = nullptr;
	spa_hook capture_listener_ {};
	spa_hook playback_listener_ {};
	std::string group_;

	// ---- device state (guarded by the thread-loop lock) ----
	std::map<uint32_t, std::unique_ptr<SinkNode>> sinks_;
	std::string effective_default_;     // "default.audio.sink"
	std::string requested_default_;     // the last default we set ourselves
	std::string target_;                // real device we play to
	std::string external_default_;      // a real device the user made default while we were processing
	// GUI power is on (processTimer is running). Atomic so the notifier thread
	// can read it without the loop lock (taking that lock while holding
	// notify_mutex_ would invert the lock order deviceListChanged() uses).
	std::atomic<bool> processing_active_ { false };
	bool playback_failed_ = false;
	bool core_dead_ = false;
	int default_retry_ticks_ = 0;
	int latency_ms_ = kDefaultLatencyMs;
	int pending_seq_ = 0;
	bool sync_done_ = false;

	// ---- real-time state ----
	std::atomic<DfxDsp*> dsp_ { nullptr };
	std::atomic<bool> mute_ { false };
	std::atomic<uint32_t> rate_ { 48000 };
	uint32_t dsp_rate_ = 0;             // capture thread only
	bool playback_primed_ = false;      // playback thread only
	spa_ringbuffer ring_ {};
	float ring_data_[kRingFrames * kChannels] {};
	float scratch_[kMaxBlockFrames * kChannels] {};

	// ---- notifications ----
	std::atomic<bool> list_changed_ { true };
	std::mutex notify_mutex_;
	std::condition_variable notify_cv_;
	bool notify_pending_ = false;
	bool notifier_quit_ = false;
	AudioPassthruCallback* callback_ = nullptr;
	std::thread notifier_;

	friend class AudioPassthru;
};

// ===========================================================================
// Event tables
// ===========================================================================
const pw_core_events* AudioPassthruPrivate::coreEvents()
{
	static const pw_core_events e = [] {
		pw_core_events v {}; v.version = PW_VERSION_CORE_EVENTS;
		v.done = &AudioPassthruPrivate::onCoreDone;
		v.error = &AudioPassthruPrivate::onCoreError;
		return v; }();
	return &e;
}

const pw_registry_events* AudioPassthruPrivate::registryEvents()
{
	static const pw_registry_events e = [] {
		pw_registry_events v {}; v.version = PW_VERSION_REGISTRY_EVENTS;
		v.global = &AudioPassthruPrivate::onRegistryGlobal;
		v.global_remove = &AudioPassthruPrivate::onRegistryGlobalRemove;
		return v; }();
	return &e;
}

const pw_node_events* AudioPassthruPrivate::nodeEvents()
{
	static const pw_node_events e = [] {
		pw_node_events v {}; v.version = PW_VERSION_NODE_EVENTS;
		v.info = &AudioPassthruPrivate::onNodeInfo;
		return v; }();
	return &e;
}

const pw_metadata_events* AudioPassthruPrivate::metadataEvents()
{
	static const pw_metadata_events e = [] {
		pw_metadata_events v {}; v.version = PW_VERSION_METADATA_EVENTS;
		v.property = &AudioPassthruPrivate::onMetadataProperty;
		return v; }();
	return &e;
}

const pw_stream_events* AudioPassthruPrivate::captureEvents()
{
	static const pw_stream_events e = [] {
		pw_stream_events v {}; v.version = PW_VERSION_STREAM_EVENTS;
		v.process = &AudioPassthruPrivate::onCaptureProcess;
		v.param_changed = &AudioPassthruPrivate::onCaptureParamChanged;
		return v; }();
	return &e;
}

const pw_stream_events* AudioPassthruPrivate::playbackEvents()
{
	static const pw_stream_events e = [] {
		pw_stream_events v {}; v.version = PW_VERSION_STREAM_EVENTS;
		v.process = &AudioPassthruPrivate::onPlaybackProcess;
		v.state_changed = &AudioPassthruPrivate::onPlaybackStateChanged;
		return v; }();
	return &e;
}

// ===========================================================================
// Lifetime
// ===========================================================================
AudioPassthruPrivate::AudioPassthruPrivate()
{
	pw_init(nullptr, nullptr);
	spa_ringbuffer_init(&ring_);
	group_ = "fxsound-" + std::to_string((long)getpid());
	notifier_ = std::thread([this] { notifierThread(); });
}

AudioPassthruPrivate::~AudioPassthruPrivate()
{
	{
		std::lock_guard<std::mutex> l(notify_mutex_);
		notifier_quit_ = true;
		callback_ = nullptr;
	}
	notify_cv_.notify_all();
	if (notifier_.joinable()) notifier_.join();

	if (loop_) {
		pw_thread_loop_lock(loop_);
		if (core_ && !core_dead_) {
			// Don't leave the system default pointing at a sink that is about to vanish.
			if (effective_default_ == FXSOUND_SINK_NODE_NAME || requested_default_ == FXSOUND_SINK_NODE_NAME) {
				if (findSinkLocked(target_)) {
					requestDefaultLocked(target_);
					waitSyncLocked();
				}
			}
		}
		destroyPlaybackLocked();
		if (capture_) { spa_hook_remove(&capture_listener_); pw_stream_destroy(capture_); capture_ = nullptr; }
		for (auto& kv : sinks_) {
			if (kv.second->proxy) { spa_hook_remove(&kv.second->listener); pw_proxy_destroy(kv.second->proxy); }
		}
		sinks_.clear();
		if (metadata_) { spa_hook_remove(&metadata_listener_); pw_proxy_destroy((pw_proxy*)metadata_); metadata_ = nullptr; }
		if (registry_) { spa_hook_remove(&registry_listener_); pw_proxy_destroy((pw_proxy*)registry_); registry_ = nullptr; }
		if (core_) { spa_hook_remove(&core_listener_); pw_core_disconnect(core_); core_ = nullptr; }
		pw_thread_loop_unlock(loop_);
		pw_thread_loop_stop(loop_);
	}
	if (context_) pw_context_destroy(context_);
	if (loop_) pw_thread_loop_destroy(loop_);
	pw_deinit();
}

int AudioPassthruPrivate::init()
{
	if (loop_) return 0;

	loop_ = pw_thread_loop_new("fxsound-pipewire", nullptr);
	if (!loop_) return 1;
	context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
	if (!context_) return 1;
	if (pw_thread_loop_start(loop_) != 0) return 1;

	pw_thread_loop_lock(loop_);
	core_ = pw_context_connect(context_, nullptr, 0);
	if (!core_) {
		pw_thread_loop_unlock(loop_);
		fprintf(stderr, "[fxsound] cannot connect to PipeWire: is the pipewire service running?\n");
		return 1;
	}
	pw_core_add_listener(core_, &core_listener_, coreEvents(), this);
	registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(registry_, &registry_listener_, registryEvents(), this);

	// Two round trips: the first delivers the registry globals, the second the
	// node info / metadata values for the objects bound during the first.
	waitSyncLocked();
	waitSyncLocked();

	createCaptureStreamLocked();

	// Until the GUI picks an output, target the current system default.
	if (target_.empty()) {
		if (isRealSinkLocked(effective_default_)) target_ = effective_default_;
		else for (auto& kv : sinks_) if (isRealSinkLocked(kv.second->name)) { target_ = kv.second->name; break; }
	}
	// Wait until our own sink is registered before reporting success: the GUI
	// checks for the FxSound device right after init() and shows a device error
	// if it isn't listed yet. One round trip is not always enough (e.g. at
	// login, when PipeWire is busy), so poll for up to ~3 seconds.
	for (int i = 0; i < 60 && !core_dead_ && !findSinkLocked(FXSOUND_SINK_NODE_NAME); i++) {
		waitSyncLocked();
		if (findSinkLocked(FXSOUND_SINK_NODE_NAME)) break;
		pw_thread_loop_unlock(loop_);
		usleep(50 * 1000);
		pw_thread_loop_lock(loop_);
	}
	if (!findSinkLocked(FXSOUND_SINK_NODE_NAME))
		fprintf(stderr, "[fxsound] the FxSound sink did not appear in PipeWire\n");
	pw_thread_loop_unlock(loop_);
	return 0;
}

void AudioPassthruPrivate::waitSyncLocked()
{
	if (!core_ || core_dead_) return;
	sync_done_ = false;
	pending_seq_ = pw_core_sync(core_, PW_ID_CORE, pending_seq_);
	while (!sync_done_) {
		if (pw_thread_loop_timed_wait(loop_, 2) != 0) break;   // 2 s safety timeout
	}
}

// ===========================================================================
// Core / registry / metadata events (thread-loop thread, lock held)
// ===========================================================================
void AudioPassthruPrivate::onCoreDone(void* data, uint32_t id, int seq)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	if (id == PW_ID_CORE && seq == self->pending_seq_) {
		self->sync_done_ = true;
		pw_thread_loop_signal(self->loop_, false);
	}
}

void AudioPassthruPrivate::onCoreError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	fprintf(stderr, "[fxsound] PipeWire error id:%u res:%d: %s\n", id, res, message ? message : "");
	if (id == PW_ID_CORE && res == -EPIPE) {
		self->core_dead_ = true;
		self->sync_done_ = true;
		pw_thread_loop_signal(self->loop_, false);
		self->deviceListChanged();
	}
}

void AudioPassthruPrivate::onRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/,
                                            const char* type, uint32_t /*version*/, const struct spa_dict* props)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	if (!props) return;

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		if (!media_class || strcmp(media_class, "Audio/Sink") != 0) return;

		auto node = std::make_unique<SinkNode>();
		node->id = id;
		node->owner = self;
		const char* v;
		if ((v = spa_dict_lookup(props, PW_KEY_NODE_NAME))) node->name = v;
		if ((v = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION))) node->description = v;
		if ((v = spa_dict_lookup(props, PW_KEY_NODE_NICK))) node->nick = v;
		if (node->name.empty()) return;

		// Bind to get the full properties (channel count, form factor).
		node->proxy = (pw_proxy*)pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
		if (node->proxy)
			pw_node_add_listener((pw_node*)node->proxy, &node->listener, nodeEvents(), node.get());
		self->sinks_[id] = std::move(node);
		self->deviceListChanged();
	}
	else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0 || self->metadata_) return;
		self->metadata_ = (pw_metadata*)pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
		self->metadata_id_ = id;
		if (self->metadata_)
			pw_metadata_add_listener(self->metadata_, &self->metadata_listener_, metadataEvents(), self);
	}
}

void AudioPassthruPrivate::onRegistryGlobalRemove(void* data, uint32_t id)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	auto it = self->sinks_.find(id);
	if (it != self->sinks_.end()) {
		if (it->second->proxy) { spa_hook_remove(&it->second->listener); pw_proxy_destroy(it->second->proxy); }
		self->sinks_.erase(it);
		self->deviceListChanged();
	}
	if (id == self->metadata_id_ && self->metadata_) {
		spa_hook_remove(&self->metadata_listener_);
		pw_proxy_destroy((pw_proxy*)self->metadata_);
		self->metadata_ = nullptr;
		self->metadata_id_ = SPA_ID_INVALID;
	}
}

void AudioPassthruPrivate::onNodeInfo(void* data, const struct pw_node_info* info)
{
	auto* node = static_cast<SinkNode*>(data);
	if (!info) return;
	if (info->props) {
		const char* v;
		if ((v = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS))) node->channels = atoi(v);
		if ((v = spa_dict_lookup(info->props, PW_KEY_DEVICE_FORM_FACTOR))) node->form_factor = v;
		if ((v = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION))) node->description = v;
		if ((v = spa_dict_lookup(info->props, PW_KEY_NODE_NICK))) node->nick = v;
	}
	if (node->channels <= 0 && info->n_input_ports > 0) node->channels = (int)info->n_input_ports;
	node->owner->deviceListChanged();
}

int AudioPassthruPrivate::onMetadataProperty(void* data, uint32_t subject, const char* key,
                                             const char* /*type*/, const char* value)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	if (subject != PW_ID_CORE || !key) return 0;
	if (strcmp(key, "default.audio.sink") != 0) return 0;

	std::string name = jsonName(value);
	if (name == self->effective_default_) return 0;
	self->effective_default_ = name;

	// While processing, the user switching the system output to a real device
	// means "play FxSound through that device" (as on Windows). processTimer()
	// retargets and reclaims the default.
	if (self->processing_active_ && self->isRealSinkLocked(name) && name != self->requested_default_)
		self->external_default_ = name;

	self->deviceListChanged();
	return 0;
}

// ===========================================================================
// Streams
// ===========================================================================
static const spa_pod* buildFormat(spa_pod_builder* b)
{
	spa_audio_info_raw info {};
	info.format = SPA_AUDIO_FORMAT_F32;
	info.channels = kChannels;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	// rate left at 0: use the graph rate, so no resampling happens here.
	return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

static std::string latencyString(int ms)
{
	int frames = 48 * (ms > 0 ? ms : kDefaultLatencyMs);
	return std::to_string(frames) + "/48000";
}

bool AudioPassthruPrivate::createCaptureStreamLocked()
{
	if (capture_ || !core_ || core_dead_) return capture_ != nullptr;
	pw_properties* props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CLASS, "Audio/Sink",
		PW_KEY_NODE_NAME, FXSOUND_SINK_NODE_NAME,
		PW_KEY_NODE_DESCRIPTION, "FxSound Audio Enhancer",
		PW_KEY_NODE_GROUP, group_.c_str(),
		"node.link-group", group_.c_str(),
		PW_KEY_NODE_LATENCY, latencyString(latency_ms_).c_str(),
		SPA_KEY_AUDIO_POSITION, "[ FL FR ]",
		"device.icon-name", "audio-card",
		nullptr);
	capture_ = pw_stream_new(core_, "FxSound Audio Enhancer", props);
	if (!capture_) return false;
	pw_stream_add_listener(capture_, &capture_listener_, captureEvents(), this);

	uint8_t buffer[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const spa_pod* params[1] = { buildFormat(&b) };
	int res = pw_stream_connect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY,
		(pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
		params, 1);
	if (res < 0) {
		fprintf(stderr, "[fxsound] cannot create the FxSound sink: %s\n", spa_strerror(res));
		spa_hook_remove(&capture_listener_);
		pw_stream_destroy(capture_);
		capture_ = nullptr;
		return false;
	}
	return true;
}

void AudioPassthruPrivate::connectPlaybackLocked()
{
	if (playback_ || target_.empty() || !core_ || core_dead_) return;
	pw_properties* props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_NODE_NAME, "fxsound_output",
		PW_KEY_NODE_DESCRIPTION, "FxSound output",
		PW_KEY_NODE_GROUP, group_.c_str(),
		"node.link-group", group_.c_str(),
		PW_KEY_TARGET_OBJECT, target_.c_str(),
		PW_KEY_NODE_DONT_RECONNECT, "true",   // the GUI chooses the next device, as on Windows
		PW_KEY_NODE_LATENCY, latencyString(latency_ms_).c_str(),
		SPA_KEY_AUDIO_POSITION, "[ FL FR ]",
		nullptr);
	playback_ = pw_stream_new(core_, "FxSound output", props);
	if (!playback_) { playback_failed_ = true; return; }
	pw_stream_add_listener(playback_, &playback_listener_, playbackEvents(), this);

	uint8_t buffer[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const spa_pod* params[1] = { buildFormat(&b) };
	int res = pw_stream_connect(playback_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		(pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
		params, 1);
	playback_failed_ = res < 0;
	if (res < 0) {
		fprintf(stderr, "[fxsound] cannot connect playback to %s: %s\n", target_.c_str(), spa_strerror(res));
		destroyPlaybackLocked();
		playback_failed_ = true;
	}
}

void AudioPassthruPrivate::destroyPlaybackLocked()
{
	if (!playback_) return;
	spa_hook_remove(&playback_listener_);
	pw_stream_destroy(playback_);
	playback_ = nullptr;
}

void AudioPassthruPrivate::onCaptureParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	if (id != SPA_PARAM_Format || !param) return;
	spa_audio_info_raw info {};
	if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0)
		self->rate_.store(info.rate);
}

void AudioPassthruPrivate::onPlaybackStateChanged(void* data, enum pw_stream_state /*old*/,
                                                  enum pw_stream_state state, const char* error)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	bool failed = (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED);
	if (state == PW_STREAM_STATE_ERROR && error)
		fprintf(stderr, "[fxsound] playback stream error: %s\n", error);
	if (failed != self->playback_failed_) {
		self->playback_failed_ = failed;
		self->deviceListChanged();
	}
}

// Real-time: apps' audio arrives in our sink. Process it and queue it.
void AudioPassthruPrivate::onCaptureProcess(void* data)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	pw_buffer* b = pw_stream_dequeue_buffer(self->capture_);
	if (!b) return;
	spa_data& d = b->buffer->datas[0];
	if (d.data && d.chunk) {
		uint32_t off = SPA_MIN(d.chunk->offset, d.maxsize);
		uint32_t size = SPA_MIN(d.chunk->size, d.maxsize - off);
		const float* src = (const float*)SPA_PTROFF(d.data, off, void);
		uint32_t frames = size / kFrameBytes;
		DfxDsp* dsp = self->dsp_.load(std::memory_order_acquire);
		uint32_t rate = self->rate_.load(std::memory_order_relaxed);

		while (frames > 0) {
			uint32_t n = SPA_MIN(frames, kMaxBlockFrames);
			memcpy(self->scratch_, src, (size_t)n * kFrameBytes);
			if (dsp) {
				if (self->dsp_rate_ != rate) {
					dsp->setSignalFormat(32, kChannels, (int)rate, 32);
					self->dsp_rate_ = rate;
				}
				// Same call the Windows build makes: 32-bit float, in place.
				dsp->processAudio((short int*)self->scratch_, (short int*)self->scratch_, (int)n, 0);
			}
			uint32_t widx;
			int32_t filled = spa_ringbuffer_get_write_index(&self->ring_, &widx);
			uint32_t space = (uint32_t)((int32_t)(kRingFrames * kFrameBytes) - SPA_MAX(filled, 0));
			uint32_t bytes = SPA_MIN((uint32_t)n * kFrameBytes, space);
			bytes -= bytes % kFrameBytes;
			if (bytes > 0) {
				spa_ringbuffer_write_data(&self->ring_, self->ring_data_, kRingFrames * kFrameBytes,
				                          widx & (kRingFrames * kFrameBytes - 1), self->scratch_, bytes);
				spa_ringbuffer_write_update(&self->ring_, widx + bytes);
			}
			src += (size_t)n * kChannels;
			frames -= n;
		}
	}
	pw_stream_queue_buffer(self->capture_, b);
}

// Real-time: feed the real device from the ring buffer.
void AudioPassthruPrivate::onPlaybackProcess(void* data)
{
	auto* self = static_cast<AudioPassthruPrivate*>(data);
	pw_buffer* b = pw_stream_dequeue_buffer(self->playback_);
	if (!b) return;
	spa_data& d = b->buffer->datas[0];
	if (!d.data) { pw_stream_queue_buffer(self->playback_, b); return; }

	uint32_t want = d.maxsize / kFrameBytes;
	if (b->requested) want = SPA_MIN((uint32_t)b->requested, want);
	uint32_t want_bytes = want * kFrameBytes;

	uint32_t ridx;
	int32_t filled = spa_ringbuffer_get_read_index(&self->ring_, &ridx);
	if (filled < 0) filled = 0;
	// The capture and playback streams run in the same graph cycle, but in no
	// guaranteed order. Keep a cushion of one extra period so that playback
	// running before capture in some cycle doesn't underrun (an audible
	// dropout). Start, and restart after an underrun, only once two periods
	// are buffered: costs one period of latency.
	if (!self->playback_primed_) {
		if ((uint32_t)filled >= want_bytes * 2)
			self->playback_primed_ = true;
		else
			filled = 0;   // output silence while priming
	}
	// Keep latency bounded: if more than four periods are queued (e.g. after
	// the output was disconnected for a while), drop the oldest audio.
	if ((uint32_t)filled > want_bytes * 4) {
		uint32_t skip = (uint32_t)filled - want_bytes * 2;
		skip -= skip % kFrameBytes;
		ridx += skip;
		filled -= (int32_t)skip;
		spa_ringbuffer_read_update(&self->ring_, ridx);
	}
	if (self->playback_primed_ && (uint32_t)filled < want_bytes)
		self->playback_primed_ = false;   // underrun: re-prime
	uint32_t bytes = SPA_MIN((uint32_t)filled, want_bytes);
	bytes -= bytes % kFrameBytes;
	if (bytes > 0) {
		spa_ringbuffer_read_data(&self->ring_, self->ring_data_, kRingFrames * kFrameBytes,
		                         ridx & (kRingFrames * kFrameBytes - 1), d.data, bytes);
		spa_ringbuffer_read_update(&self->ring_, ridx + bytes);
	}
	if (bytes < want_bytes) memset(SPA_PTROFF(d.data, bytes, void), 0, want_bytes - bytes);
	if (self->mute_.load(std::memory_order_relaxed)) memset(d.data, 0, want_bytes);

	d.chunk->offset = 0;
	d.chunk->stride = kFrameBytes;
	d.chunk->size = want_bytes;
	pw_stream_queue_buffer(self->playback_, b);
}

// ===========================================================================
// Helpers
// ===========================================================================
const SinkNode* AudioPassthruPrivate::findSinkLocked(const std::string& node_name) const
{
	if (node_name.empty()) return nullptr;
	for (auto& kv : sinks_) if (kv.second->name == node_name) return kv.second.get();
	return nullptr;
}

bool AudioPassthruPrivate::isRealSinkLocked(const std::string& node_name) const
{
	return node_name != FXSOUND_SINK_NODE_NAME && findSinkLocked(node_name) != nullptr;
}

void AudioPassthruPrivate::requestDefaultLocked(const std::string& node_name)
{
	requested_default_ = node_name;
	if (!metadata_ || node_name.empty()) return;
	std::string json = "{ \"name\": \"";
	for (char c : node_name) { if (c == '"' || c == '\\') json += '\\'; json += c; }
	json += "\" }";
	pw_metadata_set_property(metadata_, PW_ID_CORE, "default.configured.audio.sink", "Spa:String:JSON", json.c_str());
}

void AudioPassthruPrivate::deviceListChanged()
{
	list_changed_.store(true);
	{
		std::lock_guard<std::mutex> l(notify_mutex_);
		notify_pending_ = true;
	}
	notify_cv_.notify_one();
}

void AudioPassthruPrivate::notifierThread()
{
	std::unique_lock<std::mutex> l(notify_mutex_);
	for (;;) {
		notify_cv_.wait(l, [this] { return notify_pending_ || notifier_quit_; });
		if (notifier_quit_) return;
		// Debounce: plugging in a device produces a burst of events.
		notify_cv_.wait_for(l, std::chrono::milliseconds(150), [this] { return notifier_quit_; });
		if (notifier_quit_) return;
		notify_pending_ = false;
		AudioPassthruCallback* cb = callback_;
		if (!cb) continue;
		bool processing = processing_active_.load();
		l.unlock();
		cb->onSoundDeviceChange(processing);
		l.lock();
	}
}

// ===========================================================================
// Public API (GUI thread)
// ===========================================================================
std::vector<SoundDevice> AudioPassthruPrivate::getSoundDevices(bool /*active_devices*/)
{
	std::vector<SoundDevice> out;
	if (!loop_) return out;
	pw_thread_loop_lock(loop_);
	// PipeWire only lists sinks that exist, so every device is "active".
	for (auto& kv : sinks_) {
		const SinkNode& n = *kv.second;
		SoundDevice d;
		d.pwszID = widen(n.name.c_str());
		d.isPlaybackDevice = true;
		d.isActive = true;
		d.isDFXDevice = (n.name == FXSOUND_SINK_NODE_NAME);
		d.isRealDevice = !d.isDFXDevice;
		d.deviceFriendlyName = d.isDFXDevice ? std::wstring(FXSOUND_SINK_DESCRIPTION)
		                                     : widen((n.description.empty() ? n.name : n.description).c_str());
		d.deviceDescription = widen(n.nick.empty() ? "Speakers" : n.nick.c_str());
		d.deviceFormFactor = widen(n.form_factor.c_str());
		d.deviceNumChannel = n.channels > 0 ? n.channels : 2;
		d.isDefaultDevice = (n.name == effective_default_);
		d.isTargetedRealPlaybackDevice = d.isRealDevice && (n.name == target_);
		d.isUserSelectedPlaybackDevice = d.isTargetedRealPlaybackDevice;
		if (d.isRealDevice) {
			d.pwszIDRealDevices = d.pwszID;
			d.deviceFriendlyNameRealDevices = d.deviceFriendlyName;
			d.deviceDescriptionRealDevices = d.deviceDescription;
		}
		out.push_back(std::move(d));
	}
	pw_thread_loop_unlock(loop_);
	return out;
}

int AudioPassthruPrivate::setBufferLength(int ms)
{
	if (ms <= 0) return 1;
	if (!loop_) { latency_ms_ = ms; return 0; }
	pw_thread_loop_lock(loop_);
	latency_ms_ = ms;
	// Takes effect when the streams are next (re)created.
	pw_thread_loop_unlock(loop_);
	return 0;
}

int AudioPassthruPrivate::processTimer()
{
	if (!loop_) return 1;
	pw_thread_loop_lock(loop_);
	processing_active_ = true;
	if (core_dead_) { pw_thread_loop_unlock(loop_); return 1; }

	if (!capture_) createCaptureStreamLocked();

	// The user picked a different real output in the system settings.
	if (!external_default_.empty()) {
		if (isRealSinkLocked(external_default_) && external_default_ != target_) {
			target_ = external_default_;
			destroyPlaybackLocked();
		}
		external_default_.clear();
		requested_default_.clear();   // force reclaiming below
		deviceListChanged();
	}

	if (!findSinkLocked(target_)) {
		// Target vanished and the GUI hasn't chosen yet: keep the old name so a
		// replug resumes; drop the dead stream so it reconnects when it returns.
		destroyPlaybackLocked();
	}
	else if (!playback_ || playback_failed_) {
		destroyPlaybackLocked();
		connectPlaybackLocked();
	}

	// Make FxSound the system default so all apps play through it.
	if (effective_default_ != FXSOUND_SINK_NODE_NAME) {
		if (requested_default_ != FXSOUND_SINK_NODE_NAME || ++default_retry_ticks_ >= 20) {
			requestDefaultLocked(FXSOUND_SINK_NODE_NAME);
			default_retry_ticks_ = 0;
		}
	} else {
		default_retry_ticks_ = 0;
	}
	pw_thread_loop_unlock(loop_);
	return 0;
}

void AudioPassthruPrivate::setAsPlaybackDevice(const SoundDevice& dev)
{
	if (!loop_) return;
	std::string name = narrow(dev.pwszID);
	pw_thread_loop_lock(loop_);
	if (isRealSinkLocked(name)) {
		if (name != target_) {
			target_ = name;
			destroyPlaybackLocked();
		}
		connectPlaybackLocked();
		// Processing on: route apps through FxSound. Off: FxSound acts as an
		// output switcher and makes the chosen device the system default,
		// as the Windows build does.
		requestDefaultLocked(processing_active_ ? std::string(FXSOUND_SINK_NODE_NAME) : name);
	}
	pw_thread_loop_unlock(loop_);
}

bool AudioPassthruPrivate::isPlaybackDeviceAvailable()
{
	if (!loop_) return false;
	pw_thread_loop_lock(loop_);
	bool ok = !core_dead_ && findSinkLocked(target_) != nullptr && !playback_failed_;
	pw_thread_loop_unlock(loop_);
	return ok;
}

void AudioPassthruPrivate::restoreDefaultPlaybackDevice()
{
	if (!loop_) return;
	pw_thread_loop_lock(loop_);
	processing_active_ = false;
	external_default_.clear();
	if (isRealSinkLocked(target_)) requestDefaultLocked(target_);
	pw_thread_loop_unlock(loop_);
}

// ===========================================================================
// AudioPassthru (public wrapper, same as the Windows build)
// ===========================================================================
AudioPassthru::AudioPassthru() : data_(new AudioPassthruPrivate()) {}
AudioPassthru::~AudioPassthru() { delete data_; }
int AudioPassthru::init() { return data_->init(); }
void AudioPassthru::mute(bool m) { data_->mute(m); }
std::vector<SoundDevice> AudioPassthru::getSoundDevices(bool active_devices) { return data_->getSoundDevices(active_devices); }
int AudioPassthru::setBufferLength(int ms) { return data_->setBufferLength(ms); }
int AudioPassthru::processTimer() { return data_->processTimer(); }
void AudioPassthru::setDspProcessingModule(DfxDsp* dsp) { data_->setDspProcessingModule(dsp); }
void AudioPassthru::setAsPlaybackDevice(const SoundDevice sound_device) { data_->setAsPlaybackDevice(sound_device); }
void AudioPassthru::registerCallback(AudioPassthruCallback* cb) { data_->registerCallback(cb); }
bool AudioPassthru::isPlaybackDeviceAvailable() { return data_->isPlaybackDeviceAvailable(); }
bool AudioPassthru::checkDeviceChanges() { return data_->checkDeviceChanges(); }
void AudioPassthru::restoreDefaultPlaybackDevice() { data_->restoreDefaultPlaybackDevice(); }
