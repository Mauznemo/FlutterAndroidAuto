// Where PCM goes once it has been unwrapped from the protocol.
//
// The API agnostic seam for audio, the same idea as frame_ring.h is for video: nothing
// above this line names PulseAudio, and nothing below it knows what a channel is. One
// backend exists today, libpulse-simple, which on this machine is PipeWire wearing
// PulseAudio's API. ALSA or a direct PipeWire backend would be another file and no
// change anywhere else.
//
// Every method is blocking and none of them is thread safe. A sink belongs to exactly
// one thread, which is never an io_context thread: Write() waits for the server to take
// the samples, which is how the head unit is paced to real time, and waiting on an io
// thread stalls the USB transport. AudioOutput owns that thread.

#ifndef ANDROID_AUTO_LINUX_AUDIO_PCM_SINK_H_
#define ANDROID_AUTO_LINUX_AUDIO_PCM_SINK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aa {

// One PCM stream's shape. Android Auto only ever sends signed 16 bit little endian, so
// there is no sample format here: `bits` is carried to make a mismatch loud rather than
// to make anything configurable.
struct PcmFormat {
  int32_t sample_rate = 48000;
  int32_t channels = 2;
  int32_t bits = 16;

  bool operator==(const PcmFormat& other) const {
    return sample_rate == other.sample_rate && channels == other.channels &&
           bits == other.bits;
  }
  bool operator!=(const PcmFormat& other) const { return !(*this == other); }

  // Bytes one second of this stream occupies, for turning buffer sizes into durations.
  int32_t bytes_per_second() const { return sample_rate * channels * (bits / 8); }
};

// An output the host app can pick, as reported by the audio server.
struct PcmDevice {
  // What to hand back to PcmSink::Open. Stable across reboots, not human friendly.
  std::string name;
  // What to show a person: "Built-in Audio Analogue Stereo".
  std::string description;
  // Whether the server would have picked this one anyway.
  bool is_default = false;
};

class PcmSink {
 public:
  virtual ~PcmSink() = default;

  // Connects to the audio server and starts a stream. `device` is a PcmDevice::name, or
  // empty for whatever the server thinks is the default. `label` is what shows up in a
  // volume mixer, so it should say which of the three streams this is.
  //
  // Returns false and fills `error` rather than throwing: a head unit whose speakers
  // are missing should still project.
  virtual bool Open(const PcmFormat& format, const std::string& device,
                    const std::string& label, std::string* error) = 0;

  // Hands `size` bytes of interleaved PCM to the server. Blocks until they are taken,
  // which past the first buffer means roughly real time. False means the stream broke
  // and the caller should close and reopen it.
  virtual bool Write(const uint8_t* data, size_t size) = 0;

  // Throws away whatever has not been played yet. Used when the phone stops a stream,
  // so a resume does not begin with a second of stale audio.
  virtual void Flush() = 0;

  // Plays out what is already buffered, then returns. Used on a clean stop.
  virtual void Drain() = 0;

  virtual void Close() = 0;

  // How far behind the write head the speakers are, in microseconds, or 0 if the
  // backend cannot say.
  virtual int64_t LatencyMicros() = 0;

  // Which backend this is, for logs and for the Dart side to report.
  virtual std::string backend_name() const = 0;
};

// Builds the backend this machine has. Never returns null.
std::unique_ptr<PcmSink> CreatePcmSink();

// A sink that accepts samples and plays nothing, for a machine with no audio server.
// AudioOutput falls back to it when an open fails, so a head unit with no speakers
// behaves identically to one with them everywhere except the speakers.
std::unique_ptr<PcmSink> CreateNullPcmSink();

// What the audio server is currently offering. Empty if it cannot be reached at all.
// Blocks for up to a second, so call it from the platform thread, not an io thread.
std::vector<PcmDevice> ListPcmDevices();

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_AUDIO_PCM_SINK_H_
