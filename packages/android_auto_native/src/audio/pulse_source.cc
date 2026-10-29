// SPDX-License-Identifier: GPL-3.0-or-later
// The PcmSource backend built on libpulse-simple, plus input enumeration on libpulse.
//
// The capture half of pulse_sink.cc, and the same reasoning: on most current Linux
// systems PulseAudio is PipeWire answering to PulseAudio's API, so one piece of code
// covers a PipeWire desktop, a PulseAudio one, and the compatibility layer infotainment
// images ship.
//
// pa_simple is blocking by design and that is the point again, in the other direction:
// pa_simple_read returns when the microphone has actually produced the samples, so the
// thread calling it is paced by real time and the phone gets speech at the speed it was
// spoken.
//
// The enumeration below is deliberately a second copy of the one in pulse_sink.cc rather
// than something shared. Sinks and sources are separate seams on purpose, so that a
// machine could play through one backend and capture through another, and factoring the
// mainloop plumbing out would tie the two together for the sake of sixty lines.

#include "pcm_source.h"

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

// How much audio the server is asked to hand over at a time.
//
// Small, unlike the playback side's buffer, because this is latency the Assistant pays
// twice: once waiting for the samples and once more while the phone decides the person
// has stopped talking. 32 ms is a couple of scheduling quanta, which is enough that the
// capture thread is not woken constantly, and it happens to be exactly one 1024 byte
// read at 16 kHz mono.
constexpr int64_t kFragmentMicros = 32000;

// The same application name the playback side uses, so a mixer groups the two together
// rather than showing a head unit that plays and a separate one that listens.
constexpr const char* kApplicationName = "Android Auto";

// The capture side of the same rule pulse_sink.cc explains in full: a head unit's
// microphone is car hardware, so an empty device name must not follow the audio
// server's default onto a Bluetooth device. Without this, pairing a phone for hands
// free calling would point the Assistant's microphone at the far end of a call instead
// of at the person in the car.
bool IsBluetoothDevice(const std::string& name) {
  return name.rfind("bluez_", 0) == 0;
}

std::string DefaultHeadUnitDevice() {
  std::string first_wired;
  bool bluetooth_default = false;
  for (const PcmDevice& device : ListPcmCaptureDevices()) {
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
    AASDK_LOG(info) << "[Microphone] the audio server's default input is a Bluetooth "
                       "device, capturing from "
                    << first_wired << " instead";
  }
  return first_wired;
}

class PulseSource : public PcmSource {
 public:
  ~PulseSource() override { Close(); }

  bool Open(const PcmFormat& format, const std::string& device, const std::string& label,
            std::string* error) override {
    Close();

    if (format.bits != 16) {
      if (error != nullptr) {
        *error = "only 16 bit PCM is supported, the phone asked for " +
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
    // fragsize is the recording side's tlength: how much the server accumulates before
    // handing it over. Everything else is meaningless for a record stream and is left
    // to the server.
    attr.fragsize = pa_usec_to_bytes(kFragmentMicros, &spec);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);

    // An empty name means the head unit's own microphone rather than whatever the
    // server currently points at, so it is resolved to a concrete device here.
    const std::string target = device.empty() ? DefaultHeadUnitDevice() : device;

    int code = 0;
    stream_ = pa_simple_new(nullptr, kApplicationName, PA_STREAM_RECORD,
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

  bool Read(uint8_t* data, size_t size) override {
    if (stream_ == nullptr || size == 0) {
      return false;
    }
    int code = 0;
    return pa_simple_read(stream_, data, size, &code) >= 0;
  }

  void Close() override {
    if (stream_ != nullptr) {
      pa_simple_free(stream_);
      stream_ = nullptr;
    }
  }

  std::string backend_name() const override { return "PulseAudio"; }

 private:
  pa_simple* stream_ = nullptr;
};

// A source for a machine with no microphone. Produces silence, paced as a real one would
// be, so the phone's Assistant times out the way it would against a muted microphone
// rather than waiting on a channel that never speaks.
class SilentSource : public PcmSource {
 public:
  bool Open(const PcmFormat& format, const std::string&, const std::string&,
            std::string*) override {
    bytes_per_second_ = format.bytes_per_second();
    return true;
  }

  bool Read(uint8_t* data, size_t size) override {
    if (data == nullptr || size == 0 || bytes_per_second_ <= 0) {
      return false;
    }
    // Slept for, not returned at once. A caller that reads as fast as it can would spin
    // a core and flood the phone with a minute of silence a second.
    std::this_thread::sleep_for(std::chrono::microseconds(
        static_cast<int64_t>(size) * 1000000 / bytes_per_second_));
    std::memset(data, 0, size);
    return true;
  }

  void Close() override {}

  std::string backend_name() const override { return "none"; }

 private:
  int32_t bytes_per_second_ = 32000;
};

// Everything the enumeration below carries between libpulse's callbacks, which are C
// function pointers with one void* between them.
struct SourceQuery {
  pa_mainloop* mainloop = nullptr;
  std::vector<PcmDevice> devices;
  std::string default_source;
  bool failed = false;
  bool done = false;
};

void OnServerInfo(pa_context*, const pa_server_info* info, void* userdata) {
  auto* query = static_cast<SourceQuery*>(userdata);
  if (info != nullptr && info->default_source_name != nullptr) {
    query->default_source = info->default_source_name;
  }
}

void OnSourceInfo(pa_context*, const pa_source_info* info, int eol, void* userdata) {
  auto* query = static_cast<SourceQuery*>(userdata);
  if (eol != 0) {
    for (PcmDevice& device : query->devices) {
      device.is_default = device.name == query->default_source;
    }
    query->done = true;
    pa_mainloop_quit(query->mainloop, 0);
    return;
  }
  if (info == nullptr) {
    return;
  }
  // Every output has a monitor source, and offering those as microphones would let a
  // head unit send the phone its own audio back. They are not inputs in any sense a
  // person means by the word, so they are not listed.
  if (info->monitor_of_sink != PA_INVALID_INDEX) {
    return;
  }
  PcmDevice device;
  device.name = info->name == nullptr ? std::string() : info->name;
  device.description =
      info->description == nullptr ? device.name : std::string(info->description);
  query->devices.push_back(std::move(device));
}

void OnContextState(pa_context* context, void* userdata) {
  auto* query = static_cast<SourceQuery*>(userdata);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      pa_operation_unref(pa_context_get_server_info(context, OnServerInfo, query));
      pa_operation_unref(pa_context_get_source_info_list(context, OnSourceInfo, query));
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

std::unique_ptr<PcmSource> CreatePcmSource() {
  // The silent source is not chosen here, for the same reason the null sink is not
  // chosen in CreatePcmSink: pa_simple_new is what discovers there is no microphone, and
  // it does that on the capture thread where a blocking connect is free. AudioInput
  // falls back when an open fails, so a microphone plugged in later is picked up the
  // next time the phone asks rather than written off for the life of the process.
  return std::make_unique<PulseSource>();
}

std::unique_ptr<PcmSource> CreateSilentPcmSource() {
  return std::make_unique<SilentSource>();
}

std::vector<PcmDevice> ListPcmCaptureDevices() {
  SourceQuery query;
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
    // Iterated by hand rather than pa_mainloop_run, so a server that accepts the
    // connection and then says nothing cannot hang the platform thread. See the same
    // loop in pulse_sink.cc.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!query.done && !query.failed &&
           std::chrono::steady_clock::now() < deadline) {
      if (pa_mainloop_iterate(query.mainloop, 0, nullptr) < 0) {
        break;
      }
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
