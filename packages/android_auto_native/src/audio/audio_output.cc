// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_output.h"

#include <algorithm>
#include <chrono>

#include <aasdk/Common/Log.hpp>

namespace aa {
namespace {

int64_t SteadyMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// How often the summary line goes out while a stream is playing. The figure that
// matters, underruns over a long playback, is invisible without it and far too dull to
// print per buffer.
constexpr int64_t kReportIntervalMicros = 30000000;

// Latency is read every few writes rather than every one. It is a cheap call, but a
// cheap call at fifty a second on three threads is not free either.
constexpr int kLatencyCheckInterval = 8;

// One dry spell is one underrun, however many writes notice it.
constexpr int64_t kUnderrunDebounceMicros = 250000;

}  // namespace

PcmFormat DefaultFormatFor(AudioStream stream) {
  // Exactly what service_discovery.cc advertises for each sink, which is the only thing
  // the phone will ever send. A mismatch here is a stream that opens at the wrong rate
  // and plays at the wrong pitch, so the two live next to each other on purpose.
  switch (stream) {
    case AudioStream::kMedia:
      return PcmFormat{48000, 2, 16};
    case AudioStream::kSystem:
    case AudioStream::kSpeech:
    default:
      return PcmFormat{16000, 1, 16};
  }
}

const char* AudioStreamName(AudioStream stream) {
  switch (stream) {
    case AudioStream::kMedia:
      return "media audio";
    case AudioStream::kSystem:
      return "system audio";
    case AudioStream::kSpeech:
      return "speech audio";
    default:
      return "audio";
  }
}

AudioOutput::AudioOutput(LogHandler log) : log_(std::move(log)) {
  for (int i = 0; i < kAudioStreamCount; ++i) {
    tracks_[i].id = static_cast<AudioStream>(i);
    tracks_[i].format = DefaultFormatFor(tracks_[i].id);
  }
}

AudioOutput::~AudioOutput() { Stop(); }

AudioOutput::Track& AudioOutput::At(AudioStream stream) {
  int index = static_cast<int>(stream);
  if (index < 0 || index >= kAudioStreamCount) {
    index = 0;
  }
  return tracks_[index];
}

const AudioOutput::Track& AudioOutput::At(AudioStream stream) const {
  int index = static_cast<int>(stream);
  if (index < 0 || index >= kAudioStreamCount) {
    index = 0;
  }
  return tracks_[index];
}

void AudioOutput::Start() {
  if (running_.exchange(true)) {
    return;
  }
  for (Track& track : tracks_) {
    {
      std::lock_guard<std::mutex> lock(track.mutex);
      track.quit = false;
    }
    track.thread = std::thread([this, &track]() { Run(track); });
  }
}

void AudioOutput::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  for (Track& track : tracks_) {
    {
      std::lock_guard<std::mutex> lock(track.mutex);
      track.quit = true;
      track.active = false;
      track.drain_pending = false;
      track.queue.clear();
      track.queued_bytes = 0;
    }
    track.cv.notify_all();
  }
  for (Track& track : tracks_) {
    if (track.thread.joinable()) {
      // A writer can be inside a blocking write, which returns once the server has
      // taken that buffer: at most the target latency, so a fraction of a second.
      track.thread.join();
    }
  }
}

void AudioOutput::BeginStream(AudioStream stream, const PcmFormat& format) {
  Track& track = At(stream);
  {
    std::lock_guard<std::mutex> lock(track.mutex);
    track.format = format;
    track.active = true;
    track.drain_pending = false;
  }
  track.cv.notify_one();
}

void AudioOutput::EndStream(AudioStream stream) {
  Track& track = At(stream);
  {
    std::lock_guard<std::mutex> lock(track.mutex);
    track.active = false;
    // Not a flush. The phone ends a spoken instruction with the stop indication, so
    // discarding what is queued clips the last syllable off every guidance prompt.
    track.drain_pending = true;
  }
  track.cv.notify_one();
}

void AudioOutput::Submit(AudioStream stream, const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || !running_.load()) {
    return;
  }
  Track& track = At(stream);
  {
    std::lock_guard<std::mutex> lock(track.mutex);
    // Checked again under the lock, not only through running_ above: Stop() clears the
    // queue and then joins, so a buffer that slipped past the atomic would sit in the
    // queue until the next session and be played as a moment of the previous phone.
    if (track.quit) {
      return;
    }
    track.queue.emplace_back(data, data + size);
    track.queued_bytes += size;

    const size_t limit = static_cast<size_t>(
        static_cast<int64_t>(track.format.bytes_per_second()) * kMaxQueuedMicros /
        1000000);
    // Oldest first. A head unit that is behind should catch up to the picture rather
    // than refuse what is being sent now.
    while (track.queued_bytes > limit && track.queue.size() > 1) {
      track.queued_bytes -= track.queue.front().size();
      track.queue.pop_front();
      ++track.dropped;
    }
  }
  track.cv.notify_one();
}

void AudioOutput::SetVolume(AudioStream stream, double volume) {
  At(stream).volume = std::clamp(volume, 0.0, 1.0);
}

double AudioOutput::Volume(AudioStream stream) const { return At(stream).volume.load(); }

void AudioOutput::SetMuted(AudioStream stream, bool muted) {
  At(stream).muted = muted;
}

bool AudioOutput::Muted(AudioStream stream) const { return At(stream).muted.load(); }

void AudioOutput::SetDevice(const std::string& device) {
  {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (device_ == device) {
      return;
    }
    device_ = device;
  }
  // Nothing is reopened here. Each writer notices on its next buffer, which for a
  // stream that is playing is within a buffer and for one that is idle is when it next
  // has something to play.
  Log(device.empty() ? "Audio output moved to the system default device."
                     : "Audio output moved to " + device + ".");
}

std::string AudioOutput::device() const {
  std::lock_guard<std::mutex> lock(settings_mutex_);
  return device_;
}

void AudioOutput::SetOutputEnabled(bool enabled) {
  if (output_enabled_.exchange(enabled) == enabled) {
    return;
  }
  for (Track& track : tracks_) {
    track.cv.notify_one();
  }
  Log(enabled ? "Audio output enabled." : "Audio output disabled, the phone's PCM is "
                                          "delivered to the app and not played.");
}

void AudioOutput::SetTap(PcmTap tap) {
  std::lock_guard<std::mutex> lock(settings_mutex_);
  tap_ = std::move(tap);
}

std::string AudioOutput::backend_name() const {
  std::lock_guard<std::mutex> lock(settings_mutex_);
  return backend_;
}

int64_t AudioOutput::LatencyMicros(AudioStream stream) const {
  return At(stream).latency_us.load();
}

uint64_t AudioOutput::Underruns(AudioStream stream) const {
  return At(stream).underruns.load();
}

uint64_t AudioOutput::Dropped(AudioStream stream) const {
  return At(stream).dropped.load();
}

bool AudioOutput::Ducking() const {
  const Track& speech = tracks_[static_cast<int>(AudioStream::kSpeech)];
  const int64_t last = speech.last_audio_us.load();
  return last != 0 && SteadyMicros() - last < kDuckHoldMicros;
}

double AudioOutput::TargetGain(const Track& track) const {
  if (track.muted.load()) {
    return 0.0;
  }
  double gain = track.volume.load();
  if (track.id == AudioStream::kMedia && Ducking()) {
    // The phone cannot do this itself. Its music and its navigation prompt are already
    // on separate channels by the time they leave it, so whoever mixes them is whoever
    // ducks, and that is this head unit.
    gain *= kDuckGain;
  }
  return gain;
}

void AudioOutput::ApplyGain(Track& track, const PcmFormat& format,
                            std::vector<uint8_t>& bytes) {
  if (track.id == AudioStream::kMedia) {
    // Said once per transition, not per buffer. Whether the duck actually engaged is
    // otherwise only answerable by recording the speakers and reading the envelope,
    // which is how this was verified the first time and is not a thing to repeat.
    const bool ducked = Ducking();
    if (ducked != track.ducked) {
      track.ducked = ducked;
      AASDK_LOG(info) << "[Audio] media " << (ducked ? "ducked under speech"
                                                     : "back up, speech finished");
    }
  }
  const double target = TargetGain(track);
  if (track.gain == 1.0 && target == 1.0) {
    return;
  }
  auto* samples = reinterpret_cast<int16_t*>(bytes.data());
  const size_t count = bytes.size() / sizeof(int16_t);
  // A gain change applied as a step is a click, and a click under a navigation prompt
  // is more noticeable than the ducking it announces. This walks to the target instead,
  // over kGainRampSeconds at most, continuing across buffers.
  const double step =
      1.0 / (static_cast<double>(format.sample_rate) * format.channels * kGainRampSeconds);
  for (size_t i = 0; i < count; ++i) {
    if (track.gain < target) {
      track.gain = std::min(target, track.gain + step);
    } else if (track.gain > target) {
      track.gain = std::max(target, track.gain - step);
    }
    samples[i] = static_cast<int16_t>(samples[i] * track.gain);
  }
}

bool AudioOutput::EnsureOpen(Track& track, const PcmFormat& format) {
  std::string device;
  {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    device = device_;
  }
  if (track.open && track.open_format == format && track.open_device == device) {
    return true;
  }
  if (track.sink) {
    track.sink->Close();
  }
  // Rebuilt rather than reused, so a machine whose audio server was not running at the
  // first attempt is not written off for the life of the process.
  track.sink = CreatePcmSink();
  const std::string label = AudioStreamName(track.id);
  std::string error;
  if (!track.sink->Open(format, device, label, &error)) {
    Log("Could not open the " + label + " output: " + error +
        ". The phone will keep sending and it will not be heard.");
    track.sink = CreateNullPcmSink();
    track.sink->Open(format, device, label, nullptr);
  }
  track.open = true;
  track.open_format = format;
  track.open_device = device;
  track.primed = false;
  track.gain = TargetGain(track);
  {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    backend_ = track.sink->backend_name();
  }
  AASDK_LOG(info) << "[Audio] " << label << " open at " << format.sample_rate << " Hz, "
                  << format.channels << " channel(s), "
                  << (device.empty() ? "default device" : device) << ", backend "
                  << track.sink->backend_name();
  return true;
}

void AudioOutput::CheckLatency(Track& track, const PcmFormat& format) {
  if (++track.writes_since_latency_check < kLatencyCheckInterval) {
    return;
  }
  track.writes_since_latency_check = 0;
  const int64_t latency = track.sink->LatencyMicros();
  track.latency_us = latency;
  if (latency <= 0) {
    return;
  }
  if (latency > kHighWaterMicros) {
    track.primed = true;
  }
  // A stream the phone feeds in real time never gets ahead of the speakers, so its
  // buffer sits near empty from the first sample to the last and every reading looks
  // like a disaster. Speech is exactly that: a spoken instruction arrives as fast as it
  // is spoken. Counting those made every navigation prompt an underrun, which is why
  // the counter only starts once the buffer has been full at least once on this open.
  if (track.primed && latency < kLowWaterMicros) {
    const int64_t now = SteadyMicros();
    if (now - track.last_underrun_us > kUnderrunDebounceMicros) {
      track.last_underrun_us = now;
      ++track.underruns;
    }
  }
}

void AudioOutput::ReportPeriodically(Track& track, const PcmFormat& format) {
  const int64_t now = SteadyMicros();
  if (track.last_report_us == 0) {
    track.last_report_us = now;
    track.bytes_at_last_report = track.bytes_played.load();
    return;
  }
  if (now - track.last_report_us < kReportIntervalMicros) {
    return;
  }
  const uint64_t played = track.bytes_played.load();
  const uint64_t since = played - track.bytes_at_last_report;
  track.last_report_us = now;
  track.bytes_at_last_report = played;
  AASDK_LOG(info) << "[Audio] " << AudioStreamName(track.id) << ": "
                  << (since / std::max(1, format.bytes_per_second())) << "s played, "
                  << track.latency_us.load() / 1000 << " ms buffered, "
                  << track.underruns.load() << " underruns, " << track.dropped.load()
                  << " buffers dropped";
}

void AudioOutput::Run(Track& track) {
  for (;;) {
    std::vector<uint8_t> buffer;
    PcmFormat format;
    bool drain = false;
    {
      std::unique_lock<std::mutex> lock(track.mutex);
      track.cv.wait(lock, [&track] {
        return track.quit || !track.queue.empty() || track.drain_pending;
      });
      if (track.quit) {
        break;
      }
      format = track.format;
      if (!track.queue.empty()) {
        buffer = std::move(track.queue.front());
        track.queue.pop_front();
        track.queued_bytes -= buffer.size();
      } else if (track.drain_pending) {
        track.drain_pending = false;
        drain = true;
      }
    }

    if (drain) {
      // Everything the phone sent has been handed over; wait for the speakers to
      // finish it. Deliberately not a close: guidance comes every few seconds while
      // navigating, and reopening the stream each time costs a fresh prebuffer.
      if (track.open && track.sink) {
        track.sink->Drain();
        track.latency_us = 0;
        // The next thing to play starts from an empty buffer, so whether this one ever
        // filled says nothing about it.
        track.primed = false;
      }
      continue;
    }
    if (buffer.empty()) {
      continue;
    }

    {
      PcmTap tap;
      {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        tap = tap_;
      }
      // Before the gain, because an app mixing this itself wants what the phone sent,
      // not what this head unit decided to do with it.
      if (tap) {
        tap(track.id, format, buffer.data(), buffer.size());
      }
    }

    // Set here rather than in Submit: the writer is paced by the speakers, so this is
    // close to when the samples are actually heard, which is what media has to duck
    // against.
    track.last_audio_us = SteadyMicros();

    if (!output_enabled_.load()) {
      if (track.open && track.sink) {
        track.sink->Close();
        track.open = false;
        track.latency_us = 0;
      }
      continue;
    }

    EnsureOpen(track, format);
    ApplyGain(track, format, buffer);
    if (!track.sink->Write(buffer.data(), buffer.size())) {
      Log(std::string("The ") + AudioStreamName(track.id) +
          " output failed, reopening it.");
      track.sink->Close();
      track.open = false;
      continue;
    }
    track.bytes_played += buffer.size();
    CheckLatency(track, format);
    ReportPeriodically(track, format);
  }

  if (track.sink) {
    track.sink->Close();
    track.sink.reset();
  }
  track.open = false;
  track.latency_us = 0;
}

void AudioOutput::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

}  // namespace aa
