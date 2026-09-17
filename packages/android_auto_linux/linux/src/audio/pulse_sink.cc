// The PcmSink backend built on libpulse-simple, plus device enumeration on libpulse.
//
// On most current Linux systems PulseAudio is PipeWire answering to PulseAudio's API,
// which is why this is the first backend rather than a native PipeWire one: the same
// code runs on a PipeWire desktop, a PulseAudio one, and the PulseAudio compatibility
// layer most infotainment images ship, and it is a tenth of the code of pw_stream.
//
// pa_simple is blocking by design and that is the point. The head unit has no clock of
// its own, so being paced by the speakers is exactly right: Write() returns when the
// server has taken the samples, which past the first buffer is real time.

#include "pcm_sink.h"

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <aasdk/Common/Log.hpp>

namespace aa {
namespace {

// How much audio the server is asked to keep buffered.
//
// This is the whole underrun story in one number. Too small and every scheduling hiccup
// on the way from a USB read to the writer thread is an audible hole; too large and the
// head unit lags behind what is on the screen. 150 ms is about seven of the phone's
// media buffers, enough to ride out a stalled io thread, and small enough that a track
// skip still feels immediate.
constexpr int64_t kTargetLatencyUs = 150000;

// Everything this process plays shows up under one name in a volume mixer, with the
// individual streams named per track.
constexpr const char* kApplicationName = "Android Auto";

// Whether a device name belongs to a Bluetooth audio node. Both spellings are covered:
// PipeWire calls them bluez_output.* and bluez_input.*, PulseAudio bluez_sink.* and
// bluez_source.*.
bool IsBluetoothDevice(const std::string& name) {
  return name.rfind("bluez_", 0) == 0;
}

// What an empty device name resolves to.
//
// Not "whatever the audio server defaults to right now". A head unit's speakers are car
// hardware, and the server moves its default onto a Bluetooth device the moment one
// connects. Pairing a phone for hands free calling would otherwise drag the head unit's
// media onto the phone's own uplink: the phone would hear the music as microphone
// input. That sounds exactly like the audio path breaking and is nothing of the kind.
//
// Resolving to a concrete name rather than returning empty is what also stops an
// already open stream being moved later. A stream that named its device is not one the
// server re routes when the default changes; a stream that took the default is.
//
// Naming a Bluetooth device through aa_session_set_audio_device still works. An
// explicit choice is a decision, and this is only a guess made on the head unit's
// behalf.
//
// Empty is returned when no device can be found at all, which leaves the server to
// choose as before and lets AudioOutput fall back the way it always has.
std::string DefaultHeadUnitDevice() {
  std::string first_wired;
  bool bluetooth_default = false;
  for (const PcmDevice& device : ListPcmDevices()) {
    if (IsBluetoothDevice(device.name)) {
      bluetooth_default = bluetooth_default || device.is_default;
      continue;
    }
    if (device.is_default) {
      return device.name;
    }
    if (first_wired.empty()) {
      first_wired = device.name;
    }
  }
  if (bluetooth_default && !first_wired.empty()) {
    AASDK_LOG(info) << "[Audio] the audio server's default output is a Bluetooth "
                       "device, playing through "
                    << first_wired << " instead";
  }
  return first_wired;
}

class PulseSink : public PcmSink {
 public:
  ~PulseSink() override { Close(); }

  bool Open(const PcmFormat& format, const std::string& device, const std::string& label,
            std::string* error) override {
    Close();

    if (format.bits != 16) {
      if (error != nullptr) {
        *error = "only 16 bit PCM is supported, the phone offered " +
                 std::to_string(format.bits);
      }
      return false;
    }

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = static_cast<uint32_t>(format.sample_rate);
    spec.channels = static_cast<uint8_t>(format.channels);

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = pa_usec_to_bytes(kTargetLatencyUs, &spec);
    // Everything else left to the server. prebuf in particular is what holds playback
    // until there is enough to start smoothly, and the server picks it from tlength.
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(-1);

    // An empty name means the head unit's own speakers rather than whatever the server
    // currently points at, so it is resolved to a concrete device here.
    const std::string target = device.empty() ? DefaultHeadUnitDevice() : device;

    int code = 0;
    stream_ = pa_simple_new(nullptr, kApplicationName, PA_STREAM_PLAYBACK,
                            target.empty() ? nullptr : target.c_str(), label.c_str(),
                            &spec, nullptr, &attr, &code);
    if (stream_ == nullptr) {
      if (error != nullptr) {
        *error = pa_strerror(code);
      }
      return false;
    }
    return true;
  }

  bool Write(const uint8_t* data, size_t size) override {
    if (stream_ == nullptr || size == 0) {
      return stream_ != nullptr;
    }
    int code = 0;
    return pa_simple_write(stream_, data, size, &code) >= 0;
  }

  void Flush() override {
    if (stream_ != nullptr) {
      int code = 0;
      pa_simple_flush(stream_, &code);
    }
  }

  void Drain() override {
    if (stream_ != nullptr) {
      int code = 0;
      pa_simple_drain(stream_, &code);
    }
  }

  void Close() override {
    if (stream_ != nullptr) {
      pa_simple_free(stream_);
      stream_ = nullptr;
    }
  }

  int64_t LatencyMicros() override {
    if (stream_ == nullptr) {
      return 0;
    }
    int code = 0;
    const pa_usec_t latency = pa_simple_get_latency(stream_, &code);
    return code == 0 ? static_cast<int64_t>(latency) : 0;
  }

  std::string backend_name() const override { return "PulseAudio"; }

 private:
  pa_simple* stream_ = nullptr;
};

// A sink for a machine with no audio server. Accepts everything and plays nothing, so a
// head unit with no speakers still projects rather than failing on the audio channels.
class NullSink : public PcmSink {
 public:
  bool Open(const PcmFormat&, const std::string&, const std::string&,
            std::string*) override {
    return true;
  }
  bool Write(const uint8_t*, size_t) override { return true; }
  void Flush() override {}
  void Drain() override {}
  void Close() override {}
  int64_t LatencyMicros() override { return 0; }
  std::string backend_name() const override { return "none"; }
};

// Everything the enumeration below needs to carry between libpulse's callbacks, which
// are C function pointers with one void* between them.
struct DeviceQuery {
  pa_mainloop* mainloop = nullptr;
  std::vector<PcmDevice> devices;
  std::string default_sink;
  bool failed = false;
  bool done = false;
};

void OnServerInfo(pa_context*, const pa_server_info* info, void* userdata) {
  auto* query = static_cast<DeviceQuery*>(userdata);
  if (info != nullptr && info->default_sink_name != nullptr) {
    query->default_sink = info->default_sink_name;
  }
}

void OnSinkInfo(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
  auto* query = static_cast<DeviceQuery*>(userdata);
  if (eol != 0) {
    // The list has ended, so the default sink name collected earlier can be applied and
    // the loop let go.
    for (PcmDevice& device : query->devices) {
      device.is_default = device.name == query->default_sink;
    }
    query->done = true;
    pa_mainloop_quit(query->mainloop, 0);
    return;
  }
  if (info == nullptr) {
    return;
  }
  PcmDevice device;
  device.name = info->name == nullptr ? std::string() : info->name;
  device.description =
      info->description == nullptr ? device.name : std::string(info->description);
  query->devices.push_back(std::move(device));
}

void OnContextState(pa_context* context, void* userdata) {
  auto* query = static_cast<DeviceQuery*>(userdata);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      pa_operation_unref(pa_context_get_server_info(context, OnServerInfo, query));
      pa_operation_unref(pa_context_get_sink_info_list(context, OnSinkInfo, query));
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      query->failed = true;
      pa_mainloop_quit(query->mainloop, 0);
      break;
    default:
      break;
  }
}

}  // namespace

std::unique_ptr<PcmSink> CreatePcmSink() {
  // The null sink is not chosen here. pa_simple_new is what discovers that there is no
  // server, and it does that on the writer thread where a blocking connect is free;
  // AudioOutput falls back when an open fails, so a server that appears later is picked
  // up on the next stream rather than written off for the life of the process.
  return std::make_unique<PulseSink>();
}

std::unique_ptr<PcmSink> CreateNullPcmSink() { return std::make_unique<NullSink>(); }

std::vector<PcmDevice> ListPcmDevices() {
  DeviceQuery query;
  query.mainloop = pa_mainloop_new();
  if (query.mainloop == nullptr) {
    return {};
  }
  pa_context* context =
      pa_context_new(pa_mainloop_get_api(query.mainloop), kApplicationName);
  if (context == nullptr) {
    pa_mainloop_free(query.mainloop);
    return {};
  }

  pa_context_set_state_callback(context, OnContextState, &query);
  std::vector<PcmDevice> devices;
  if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0) {
    // Iterated by hand rather than pa_mainloop_run, so that a server which accepts the
    // connection and then says nothing cannot hang the platform thread. Its own
    // mainloop rather than a threaded one: this runs to completion on the caller's
    // thread and everything it built is gone by the time it returns, which is far
    // easier to reason about than a background loop that outlives the call.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!query.done && !query.failed &&
           std::chrono::steady_clock::now() < deadline) {
      if (pa_mainloop_iterate(query.mainloop, 0, nullptr) < 0) {
        break;
      }
      // Zero block above, so this loop would spin. A short sleep costs nothing against
      // a call that is allowed to take a second.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (query.done && !query.failed) {
      devices = std::move(query.devices);
    }
  }

  pa_context_disconnect(context);
  pa_context_unref(context);
  pa_mainloop_free(query.mainloop);
  return devices;
}

}  // namespace aa
