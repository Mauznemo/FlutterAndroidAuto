// The three audio streams a head unit plays, and the threads that play them.
//
// Android Auto sends media, system and speech as three separate PCM streams and leaves
// the mixing to the head unit. That is not a detail: the phone cannot duck its own
// navigation prompt under its own music, because by the time the two exist they are on
// different channels heading for different speakers. So the ducking lives here.
//
// One writer thread per stream, because PcmSink::Write blocks until the server takes
// the samples and blocking an io_context thread stalls the transport. Submit() is
// the seam: it is called from an io thread, copies, and returns at once.
//
// This outlives a connection, the way the video decoder does. Volume, mute and the
// chosen output device belong to the head unit rather than to whichever phone happens
// to be plugged in, and reopening an audio server connection per reconnect is a cost
// with nothing to show for it.

#ifndef ANDROID_AUTO_LINUX_AUDIO_AUDIO_OUTPUT_H_
#define ANDROID_AUTO_LINUX_AUDIO_AUDIO_OUTPUT_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pcm_sink.h"

namespace aa {

// Which of the three sinks a buffer belongs to. The integers are the C ABI's
// AaAudioStream and the Dart side's AndroidAutoAudioStream, so the three stay in step
// by sharing values rather than by anyone remembering to convert.
enum class AudioStream : int32_t {
  kMedia = 0,
  kSystem = 1,
  kSpeech = 2,
};

constexpr int kAudioStreamCount = 3;

// The format each stream is advertised with during service discovery, and therefore the
// only format the phone will send. Kept next to the enum so the two cannot drift.
PcmFormat DefaultFormatFor(AudioStream stream);

// Human readable name, for logs and for the mixer entry a listener will see.
const char* AudioStreamName(AudioStream stream);

class AudioOutput {
 public:
  using LogHandler = std::function<void(const std::string&)>;
  // Raw PCM, exactly as the phone sent it, before volume or ducking. Called on a writer
  // thread, so it must not block and must not touch Dart directly.
  using PcmTap =
      std::function<void(AudioStream, const PcmFormat&, const uint8_t*, size_t)>;

  explicit AudioOutput(LogHandler log);
  ~AudioOutput();

  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // Starts the writer threads. Idempotent. No audio server is contacted until a stream
  // actually begins, so this is cheap and cannot fail.
  void Start();

  // Stops the threads and closes the sinks. Idempotent. Safe from the platform thread:
  // a writer can be inside a blocking write, which returns within one buffer.
  void Stop();

  // Called from io threads, by AudioChannels.

  // The phone started this stream. Reopens the sink if the format changed.
  void BeginStream(AudioStream stream, const PcmFormat& format);
  // The phone stopped it. What is already queued is played out rather than dropped: a
  // guidance prompt ends with the stop indication, and discarding here cuts the last
  // syllable off every instruction.
  void EndStream(AudioStream stream);
  // One buffer of interleaved PCM. Copied, so the aasdk buffer can go away at once.
  void Submit(AudioStream stream, const uint8_t* data, size_t size);

  // Called from Flutter's platform thread, through the C ABI.

  // 0.0 to 1.0. Above 1.0 would need clipping logic for no real gain, so it is clamped.
  void SetVolume(AudioStream stream, double volume);
  double Volume(AudioStream stream) const;
  void SetMuted(AudioStream stream, bool muted);
  bool Muted(AudioStream stream) const;
  // A PcmDevice::name, or empty for the head unit's own speakers, which is the audio
  // server's default with Bluetooth devices ruled out. See DefaultHeadUnitDevice in
  // pulse_sink.cc. Takes effect on each stream the next time it opens, which for a
  // stream that is playing means at once.
  void SetDevice(const std::string& device);
  std::string device() const;
  // Turns the speakers off without touching the protocol. The phone keeps sending and
  // the tap keeps firing, which is what an app doing its own mixing wants.
  void SetOutputEnabled(bool enabled);
  bool output_enabled() const { return output_enabled_.load(); }
  void SetTap(PcmTap tap);

  // "PulseAudio", or "none" if no audio server could be reached.
  std::string backend_name() const;
  // How far behind the writer the speakers are, in microseconds.
  int64_t LatencyMicros(AudioStream stream) const;
  // Times a stream came close to running the speakers dry. The number that matters for
  // "no audible glitches"; see kLowWaterMicros.
  uint64_t Underruns(AudioStream stream) const;
  // Buffers thrown away because the phone sent faster than they could be played.
  uint64_t Dropped(AudioStream stream) const;

 private:
  // The most audio that may sit waiting for one stream. The phone is entitled to run
  // ahead, and does at the start of a track, but a queue that grows without limit is a
  // head unit that plays a minute behind the screen.
  static constexpr int64_t kMaxQueuedMicros = 1000000;
  // Below this much buffered in the server, the next hiccup is audible. Counted rather
  // than waited for, since there is nothing useful to do about it.
  static constexpr int64_t kLowWaterMicros = 25000;
  // Above this, the stream is running ahead of the speakers and a later fall really is
  // the buffer draining. See the note on Track::primed.
  static constexpr int64_t kHighWaterMicros = 60000;
  // How long after the last speech buffer the media stream stays ducked. Long enough to
  // bridge the pauses inside one spoken instruction, short enough that music is back by
  // the time the driver notices it left.
  static constexpr int64_t kDuckHoldMicros = 500000;
  // What media drops to while speech is playing.
  static constexpr double kDuckGain = 0.25;
  // How long a gain change takes to travel its full range. A step would click.
  static constexpr double kGainRampSeconds = 0.02;

  struct Track {
    AudioStream id = AudioStream::kMedia;

    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    size_t queued_bytes = 0;
    // What the phone said it would send. Guarded by mutex; the writer copies it out.
    PcmFormat format;
    bool active = false;
    bool drain_pending = false;
    bool quit = false;

    // Writer thread only.
    std::unique_ptr<PcmSink> sink;
    bool open = false;
    PcmFormat open_format;
    std::string open_device;
    double gain = 1.0;
    // Whether the last buffer was ducked, so the transition can be logged once rather
    // than per buffer. Media only.
    bool ducked = false;
    int writes_since_latency_check = 0;
    // Whether the server's buffer has ever been comfortably full on this open. Until it
    // has, a low reading means the stream is being fed in real time rather than that it
    // is about to run dry. See CheckLatency.
    bool primed = false;
    int64_t last_underrun_us = 0;
    int64_t last_report_us = 0;
    uint64_t bytes_at_last_report = 0;

    std::atomic<double> volume{1.0};
    std::atomic<bool> muted{false};
    std::atomic<int64_t> latency_us{0};
    std::atomic<int64_t> last_audio_us{0};
    std::atomic<uint64_t> bytes_played{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> dropped{0};
  };

  Track& At(AudioStream stream);
  const Track& At(AudioStream stream) const;

  void Run(Track& track);
  // Writer thread. Opens or reopens the sink if the format or the device changed.
  bool EnsureOpen(Track& track, const PcmFormat& format);
  // Writer thread. Scales `bytes` in place, ramping rather than stepping, and returns
  // whether anything still needs to be written.
  void ApplyGain(Track& track, const PcmFormat& format, std::vector<uint8_t>& bytes);
  // What `track` should be playing at right now, before the ramp.
  double TargetGain(const Track& track) const;
  // Whether speech is playing, or was recently enough that media stays ducked.
  bool Ducking() const;
  void CheckLatency(Track& track, const PcmFormat& format);
  void ReportPeriodically(Track& track, const PcmFormat& format);
  void Log(const std::string& message);

  LogHandler log_;
  Track tracks_[kAudioStreamCount];

  mutable std::mutex settings_mutex_;
  std::string device_;
  PcmTap tap_;

  // What actually opened, rather than what would be tried. Guarded by settings_mutex_
  // because the writer threads set it and the platform thread reads it.
  std::string backend_ = "none";

  std::atomic<bool> output_enabled_{true};
  std::atomic<bool> running_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_AUDIO_AUDIO_OUTPUT_H_
