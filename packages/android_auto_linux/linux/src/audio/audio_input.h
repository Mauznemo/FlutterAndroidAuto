// The microphone, and the thread that reads it.
//
// The mirror of AudioOutput, with one rule that AudioOutput does not have and that is
// the whole reason this class exists rather than a couple of lines inside the channel:
// **the capture device is opened when the phone asks for it and closed when the phone
// lets it go, and at no other time.** There is no thread sitting on an open microphone
// waiting to be needed. Start() creates the thread and the thread opens the device;
// Stop() joins it and the device closes with it, so "is this machine listening" has the
// same answer as "does this thread exist", and a person can check it in a mixer.
//
// One thread, because PcmSource::Read blocks until the microphone has produced the
// samples and blocking an io_context thread stalls the USB transport. The buffer handler
// is the seam in the other direction: it is called on the capture thread, must not
// block, and must not touch an aasdk channel in place.
//
// This outlives a connection, the way AudioOutput and the video decoder do. Which input
// to capture from belongs to the head unit rather than to whichever phone is plugged in.

#ifndef ANDROID_AUTO_LINUX_AUDIO_AUDIO_INPUT_H_
#define ANDROID_AUTO_LINUX_AUDIO_AUDIO_INPUT_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "pcm_source.h"

namespace aa {

// The format service discovery advertises for the microphone, and therefore the only
// format the phone will accept. Kept here next to the capture so the two cannot drift;
// `AddMicrophoneService` in session/service_discovery.cc is the other half.
PcmFormat MicrophoneFormat();

class AudioInput {
 public:
  using LogHandler = std::function<void(const std::string&)>;
  // One buffer of captured PCM. Called on the capture thread, so it must not block and
  // must not touch Dart directly.
  using BufferHandler = std::function<void(const uint8_t*, size_t)>;

  explicit AudioInput(LogHandler log);
  ~AudioInput();

  AudioInput(const AudioInput&) = delete;
  AudioInput& operator=(const AudioInput&) = delete;

  // Opens the microphone and starts delivering to `on_buffer`. Called from an io thread,
  // when the phone asks for the microphone, and from nowhere else. Starting twice
  // restarts: the second call's handler is the one that receives.
  void Start(const PcmFormat& format, BufferHandler on_buffer);

  // Closes the microphone and joins the thread. Idempotent, and safe from the platform
  // thread: the capture thread can be inside a blocking read, which returns within one
  // fragment.
  void Stop();

  // Whether the microphone is open right now. This is what a "the car is listening"
  // indicator shows, so it is deliberately the state of the device rather than of the
  // channel: a phone that has opened the channel but not asked to record reads false.
  bool active() const { return active_.load(); }

  // Peak of the most recent buffer, 0.0 to 1.0, for a level meter. Zero while stopped.
  double level() const { return level_.load(); }

  // Bytes handed to the handler since this object was built. Does not reset per
  // session: it answers "has this machine ever actually captured anything", which is
  // the question a silent Assistant raises.
  uint64_t bytes_captured() const { return bytes_captured_.load(); }

  // A PcmDevice::name from ListPcmCaptureDevices, or empty for the server's default.
  // Takes effect the next time the phone asks for the microphone, which is the only
  // time this is allowed to open anything.
  void SetDevice(const std::string& device);
  std::string device() const;

  // "PulseAudio", or "none" if no microphone could be opened.
  std::string backend_name() const;

 private:
  // Capture thread. Opens the source, reads until asked to quit, closes it.
  void Run(PcmFormat format, BufferHandler on_buffer);
  // Joins the capture thread. The caller must hold lifecycle_mutex_, and must not be
  // the capture thread itself.
  void StopLocked();
  void Log(const std::string& message);

  LogHandler log_;

  // Serialises Start against Stop. Start arrives on an io thread when the phone asks,
  // Stop on an io thread when it lets go and on Flutter's platform thread when the user
  // presses stop, so the two race by construction.
  std::mutex lifecycle_mutex_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> active_{false};
  std::atomic<double> level_{0.0};
  std::atomic<uint64_t> bytes_captured_{0};

  mutable std::mutex settings_mutex_;
  std::string device_;
  // What actually opened rather than what would be tried, set by the capture thread.
  std::string backend_ = "none";
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_AUDIO_AUDIO_INPUT_H_
