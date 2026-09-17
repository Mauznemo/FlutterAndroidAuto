// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_channels.h"

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace media_pb = aap_protobuf::service::media::shared::message;
namespace source_pb = aap_protobuf::service::media::source::message;

// How many buffers the phone may have in flight before it waits for an acknowledgement.
//
// This is the only backpressure on the audio path, so it is also the only thing keeping
// a phone that woke up with a full encoder from burying the head unit. Ten is what every
// other implementation asks for, and at the phone's usual buffer size it is about two
// hundred milliseconds of media: enough that a scheduling hiccup on an io thread does
// not stall the stream, small enough that the phone cannot run far ahead of what is on
// the screen.
constexpr uint32_t kMaxUnacked = 10;

AudioStream StreamFor(aasdk::messenger::ChannelId channel) {
  switch (channel) {
    case aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO:
      return AudioStream::kSystem;
    case aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO:
      return AudioStream::kSpeech;
    case aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO:
    default:
      return AudioStream::kMedia;
  }
}

}  // namespace

AudioSinkRelay::AudioSinkRelay(std::weak_ptr<AudioChannels> owner,
                               aasdk::messenger::ChannelId channel)
    : owner_(std::move(owner)), channel_(channel) {}

void AudioSinkRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnOpen(channel_);
  }
}

void AudioSinkRelay::onMediaChannelSetupRequest(const media_pb::Setup&) {
  if (auto owner = owner_.lock()) {
    owner->OnSetup(channel_);
  }
}

void AudioSinkRelay::onMediaChannelStartIndication(const media_pb::Start& indication) {
  if (auto owner = owner_.lock()) {
    owner->OnStart(channel_, indication.session_id());
  }
}

void AudioSinkRelay::onMediaChannelStopIndication(const media_pb::Stop&) {
  if (auto owner = owner_.lock()) {
    owner->OnStop(channel_);
  }
}

void AudioSinkRelay::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer& buffer) {
  if (auto owner = owner_.lock()) {
    // The phone's timestamp is on its own clock with no agreed epoch, and PCM is played
    // in the order it arrives, so there is nothing here to schedule against.
    owner->OnData(channel_, buffer);
  }
}

void AudioSinkRelay::onMediaIndication(const aasdk::common::DataConstBuffer& buffer) {
  if (auto owner = owner_.lock()) {
    owner->OnCodecConfig(channel_, buffer);
  }
}

void AudioSinkRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError(channel_, error);
  }
}

std::shared_ptr<AudioChannels> AudioChannels::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger,
    const HeadUnitDescription& description, std::shared_ptr<AudioOutput> output,
    LogHandler log) {
  return std::make_shared<AudioChannels>(io_context, strand, std::move(messenger),
                                         description, std::move(output), std::move(log));
}

AudioChannels::AudioChannels(boost::asio::io_context& io_context, aasdk::Strand& strand,
                             aasdk::messenger::IMessenger::Pointer messenger,
                             const HeadUnitDescription& description,
                             std::shared_ptr<AudioOutput> output, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      description_(description),
      output_(std::move(output)),
      log_(std::move(log)) {}

AudioChannels::~AudioChannels() { Stop(); }

void AudioChannels::Start() {
  if (description_.enable_media_audio) {
    Add(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO, AudioStream::kMedia);
  }
  if (description_.enable_system_audio) {
    Add(aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO, AudioStream::kSystem);
  }
  if (description_.enable_speech_audio) {
    Add(aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO, AudioStream::kSpeech);
  }
}

void AudioChannels::Add(aasdk::messenger::ChannelId channel, AudioStream stream) {
  {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    // No messenger means the session was torn down while it was being built. A channel
    // made now would hold a null messenger and segfault on its first receive.
    if (stopped_.load() || !messenger_) {
      return;
    }
    Sink sink;
    sink.channel =
        std::make_shared<aasdk::channel::mediasink::audio::AudioMediaSinkService>(
            strand_, messenger_, channel);
    sink.relay = std::make_shared<AudioSinkRelay>(weak_from_this(), channel);
    sink.stream = stream;
    sinks_.emplace(channel, std::move(sink));
  }
  Listen(channel);
}

void AudioChannels::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  if (output_) {
    // The link is going away, so nothing more is coming. What is already queued is
    // still played out: on a user's stop AudioOutput has been stopped first and there
    // is nothing left to play, and on a lost cable the alternative is cutting the last
    // word of whatever was being said.
    for (int i = 0; i < kAudioStreamCount; ++i) {
      output_->EndStream(static_cast<AudioStream>(i));
    }
  }
  // Moved out and destroyed after the lock is dropped, so a handler holding its own
  // references on an io thread finishes against live objects.
  std::map<aasdk::messenger::ChannelId, Sink> sinks;
  {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    sinks.swap(sinks_);
    messenger_.reset();
  }
}

AudioChannels::Sink AudioChannels::Get(aasdk::messenger::ChannelId channel) const {
  if (stopped_.load()) {
    return Sink{};
  }
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  auto found = sinks_.find(channel);
  return found == sinks_.end() ? Sink{} : found->second;
}

void AudioChannels::SetSession(aasdk::messenger::ChannelId channel, int32_t session_id) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  auto found = sinks_.find(channel);
  if (found != sinks_.end()) {
    found->second.session_id = session_id;
  }
}

void AudioChannels::MarkHeard(aasdk::messenger::ChannelId channel) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  auto found = sinks_.find(channel);
  if (found != sinks_.end()) {
    found->second.heard = true;
  }
}

void AudioChannels::Listen(aasdk::messenger::ChannelId channel) {
  Sink sink = Get(channel);
  if (sink.channel) {
    sink.channel->receive(sink.relay);
  }
}

void AudioChannels::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer AudioChannels::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`: a rejection can arrive on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<AudioChannels> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void AudioChannels::OnOpen(aasdk::messenger::ChannelId channel) {
  Sink sink = Get(channel);
  if (sink.channel) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    sink.channel->sendChannelOpenResponse(response,
                                          MakeSendPromise("audio channel open"));
    // Worth saying, because a stream that is never heard from afterwards is two
    // different problems depending on this line: a channel the phone never opened, or
    // one it opened and never used. The system sink in particular can go a whole
    // session without a single buffer.
    Log(std::string("The phone opened the ") + AudioStreamName(StreamFor(channel)) +
        " channel.");
  }
  Listen(channel);
}

void AudioChannels::OnSetup(aasdk::messenger::ChannelId channel) {
  Sink sink = Get(channel);
  if (sink.channel) {
    media_pb::Config response;
    response.set_status(media_pb::Config::STATUS_READY);
    response.set_max_unacked(kMaxUnacked);
    // Index into the audio_configs list service discovery sent for this sink. There is
    // one, which is why the phone never has to tell us the rate: it is the rate we
    // asked for, and DefaultFormatFor knows it.
    response.add_configuration_indices(0);
    sink.channel->sendChannelSetupResponse(response, MakeSendPromise("audio setup"));
  }
  Listen(channel);
}

void AudioChannels::OnStart(aasdk::messenger::ChannelId channel, int32_t session_id) {
  SetSession(channel, session_id);
  if (output_) {
    output_->BeginStream(StreamFor(channel), DefaultFormatFor(StreamFor(channel)));
  }
  AASDK_LOG(debug) << "[Audio] " << AudioStreamName(StreamFor(channel))
                   << " stream started, session " << session_id;
  Listen(channel);
}

void AudioChannels::OnStop(aasdk::messenger::ChannelId channel) {
  SetSession(channel, -1);
  if (output_) {
    output_->EndStream(StreamFor(channel));
  }
  AASDK_LOG(debug) << "[Audio] " << AudioStreamName(StreamFor(channel))
                   << " stream stopped";
  Listen(channel);
}

void AudioChannels::Acknowledge(const Sink& sink) {
  // Sent as soon as the buffer is queued rather than once it has been played. The phone
  // is entitled to kMaxUnacked buffers ahead, AudioOutput's queue is bounded, and the
  // real pacing is the writer thread waiting on the speakers. Acknowledging on playback
  // instead would put a round trip to the phone inside the audio clock.
  if (!sink.channel || sink.session_id < 0) {
    return;
  }
  source_pb::Ack ack;
  ack.set_session_id(sink.session_id);
  ack.set_ack(1);
  sink.channel->sendMediaAckIndication(ack, MakeSendPromise("audio ack"));
}

void AudioChannels::OnData(aasdk::messenger::ChannelId channel,
                           const aasdk::common::DataConstBuffer& buffer) {
  Sink sink = Get(channel);
  const AudioStream stream = StreamFor(channel);
  if (sink.channel && buffer.size > 0 && output_ != nullptr) {
    if (!sink.heard) {
      MarkHeard(channel);
      const PcmFormat format = DefaultFormatFor(stream);
      Log(std::string("First ") + AudioStreamName(stream) + " buffer received, " +
          std::to_string(format.sample_rate) + " Hz " +
          (format.channels == 1 ? "mono" : "stereo") + ".");
    }
    output_->Submit(stream, buffer.cdata, buffer.size);
  }
  Acknowledge(sink);
  Listen(channel);
}

void AudioChannels::OnCodecConfig(aasdk::messenger::ChannelId channel,
                                  const aasdk::common::DataConstBuffer& buffer) {
  // PCM has no codec configuration, so this should never arrive. It is not played and
  // not acknowledged, for the same reason video does not acknowledge its SPS and PPS:
  // it is not a buffer, so it does not count against the unacked window.
  AASDK_LOG(debug) << "[Audio] unexpected codec config on "
                   << AudioStreamName(StreamFor(channel)) << ", " << buffer.size
                   << " bytes, ignored";
  Listen(channel);
}

void AudioChannels::OnChannelError(aasdk::messenger::ChannelId channel,
                                   const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: see the note on VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log(std::string("The ") + AudioStreamName(StreamFor(channel)) +
      " channel failed: " + error.what());
}

}  // namespace aa
