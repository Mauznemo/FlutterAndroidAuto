#include "aa_core.h"

#include <boost/asio.hpp>

#include <aasdk/Common/Log.hpp>
#include <aasdk/Common/ModernLogger.hpp>
#include <aasdk/Common/Strand.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_input.h"
#include "audio/audio_output.h"
#include "audio/pcm_sink.h"
#include "audio/pcm_source.h"
#include "event_bus.h"
#include "frame_ring.h"
#include "metadata/metadata_state.h"
#include "present/gl_adapter.h"
#include "bluetooth/bluez_client.h"
#include "sensors/sensor_state.h"
#include "session/input_channel.h"
#include "session/metadata_channels.h"
#include "session/protocol_session.h"
#include "session/service_discovery.h"
#include "session/usb_connector.h"
#include "session/wireless_connector.h"
#include "test_pattern.h"
#include "video/video_decoder.h"

namespace {

// How many times to bounce an unresponsive phone before giving up and telling the user.
constexpr int kMaxRecoveryAttempts = 3;

std::string CopyOrEmpty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

// Turns an AaAudioStream integer into the enum, defaulting rather than failing: the
// three values are a closed set the Dart side cannot get wrong except by arithmetic.
bool ToAudioStream(int32_t value, aa::AudioStream* stream) {
  if (value < 0 || value >= aa::kAudioStreamCount) {
    return false;
  }
  *stream = static_cast<aa::AudioStream>(value);
  return true;
}

// The same, for an AaMetadata.
bool ToMetadata(int32_t value, aa::Metadata* which) {
  if (value < 0 || value >= aa::kMetadataCount) {
    return false;
  }
  *which = static_cast<aa::Metadata>(value);
  return true;
}

// Narrows or widens the advertised channel set, from AA_SERVICES in the environment.
//
// A phone that dislikes anything in the service discovery response rejects the whole
// thing without saying which part, so the only way to find the offending entry is to
// take channels away until it stops complaining. Rebuilding between attempts makes that
// a ten minute loop per guess; this makes it a restart.
//
//   AA_SERVICES=video                    just the projection
//   AA_SERVICES=video,input,sensor       the default
//   AA_SERVICES=all                      everything, including channels with no handler
//
// The five metadata channels are in here individually (navigation, media_status,
// phone_status, notification, browser) because they are the newest and therefore the
// first suspects when a phone that used to project stops.
//
// Unset leaves `description` as the code built it.
void ApplyServiceOverride(aa::HeadUnitDescription* description) {
  const char* services = std::getenv("AA_SERVICES");
  if (services == nullptr || *services == '\0') {
    return;
  }
  const std::string list(services);
  const bool all = list == "all";
  auto enabled = [&list, all](const char* name) {
    if (all) {
      return true;
    }
    const size_t at = list.find(name);
    if (at == std::string::npos) {
      return false;
    }
    // Guard against "audio" matching inside "media_audio": a name only counts when it
    // is a whole comma separated entry.
    const bool starts = at == 0 || list[at - 1] == ',';
    const size_t end = at + std::strlen(name);
    const bool ends = end == list.size() || list[end] == ',';
    return starts && ends;
  };

  description->enable_video = enabled("video");
  description->enable_input = enabled("input");
  // Whether the channel is there at all, not which sensors are on it. Which sensors the
  // car has is the host app's declaration and no business of an environment variable.
  if (!enabled("sensor")) {
    description->sensors = 0;
  }
  description->enable_media_audio = enabled("media_audio");
  description->enable_system_audio = enabled("system_audio");
  description->enable_speech_audio = enabled("speech_audio");
  description->enable_microphone = enabled("microphone");

  aa::MetadataMask metadata = 0;
  if (enabled("navigation")) {
    metadata |= aa::MetadataBit(aa::Metadata::kNavigation);
  }
  if (enabled("media_status")) {
    metadata |= aa::MetadataBit(aa::Metadata::kMedia);
  }
  if (enabled("phone_status")) {
    metadata |= aa::MetadataBit(aa::Metadata::kPhone);
  }
  if (enabled("notification")) {
    metadata |= aa::MetadataBit(aa::Metadata::kNotification);
  }
  if (enabled("browser")) {
    metadata |= aa::MetadataBit(aa::Metadata::kBrowse);
  }
  description->metadata = metadata;
}

// Narrows or widens the transports, from AA_TRANSPORTS in the environment.
//
// The same idea as AA_SERVICES and for the same reason: a wireless connection cannot
// be tested while the cable is plugged in, because the phone projects over the cable
// and the head unit will not replace a working session with an offered one. Unplugging
// is not always possible, and a marginal USB port is its own source of confusion.
//
//   AA_TRANSPORTS=wireless          radio only, ignore anything on the cable
//   AA_TRANSPORTS=usb,wireless      both
//
// Unset leaves whatever the host app asked for.
int32_t ApplyTransportOverride(int32_t transports) {
  const char* value = std::getenv("AA_TRANSPORTS");
  if (value == nullptr || *value == '\0') {
    return transports;
  }
  const std::string list(value);
  int32_t mask = 0;
  if (list.find("usb") != std::string::npos) {
    mask |= AA_TRANSPORT_USB;
  }
  if (list.find("wireless") != std::string::npos) {
    mask |= AA_TRANSPORT_WIRELESS;
  }
  return mask == 0 ? transports : mask;
}

// Overrides the network the phone is told to join, from the environment.
//
// Same idea as AA_TRANSPORTS: a test that brings an access point up cannot then stop
// to have its passphrase typed into a text field, and the moment that is needed is
// the moment the machine has given up its network to host the one being tested.
//
//   AA_WIRELESS_SSID=HeadUnit AA_WIRELESS_PASSPHRASE=headunit1234
//
// A passphrase in an environment variable is a test bench convenience and nothing
// more. A real head unit gets it from its host app.
void ApplyWirelessOverride(aa::WirelessSettings* settings) {
  if (const char* ssid = std::getenv("AA_WIRELESS_SSID")) {
    if (*ssid != '\0') {
      settings->ssid = ssid;
    }
  }
  if (const char* passphrase = std::getenv("AA_WIRELESS_PASSPHRASE")) {
    if (*passphrase != '\0') {
      settings->passphrase = passphrase;
    }
  }
}

// Turns aasdk's own logging up, from AA_LOG_LEVEL in the environment.
//
// aasdk already has a console sink wired up and sits at INFO, where it says almost
// nothing. Nearly every useful line about what the phone is actually doing, which
// message arrived on which channel, is at DEBUG. That is far too loud to leave on once
// video is flowing, so it is opt in:
//
//   AA_LOG_LEVEL=DEBUG dev/run-example.sh
//
// Accepted values are TRACE, DEBUG, INFO, WARN, ERROR and FATAL.
void ApplyAasdkLogLevel() {
  const char* level = std::getenv("AA_LOG_LEVEL");
  if (level == nullptr || *level == '\0') {
    return;
  }
  std::string upper(level);
  for (char& character : upper) {
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }
  aasdk::common::ModernLogger::getInstance().setLevel(
      aasdk::common::ModernLogger::stringToLevel(upper));
}

}  // namespace

// A head unit session.
//
// Owns the io_context thread pool that aasdk runs on, the video frame path, and the
// channel back to Dart. Nothing here touches Flutter's platform thread except the FFI
// entry points themselves, which Dart already calls from it.
struct AaSession {
  struct Config {
    int32_t width = 1280;
    int32_t height = 720;
    int32_t fps = 30;
    int32_t dpi = 140;
    std::string head_unit_name;
    std::string car_model;
    std::string car_year;
    aa::SensorMask sensors = aa::kRequiredSensors;
    aa::MetadataMask metadata = aa::kDefaultMetadata;
    // A cable only, until the host app asks for more. Wireless costs a Bluetooth
    // service and an open port, and no head unit should acquire either by accident.
    int32_t transports = AA_TRANSPORT_USB;
  };

  explicit AaSession(AaEventCallback callback) : events(callback) {}

  // How this head unit describes itself during service discovery.
  aa::HeadUnitDescription Describe() const {
    aa::HeadUnitDescription description;
    description.width = config.width;
    description.height = config.height;
    description.fps = config.fps;
    description.dpi = config.dpi;
    description.head_unit_name = config.head_unit_name;
    description.car_model = config.car_model;
    description.car_year = config.car_year;
    // Everything, including the channels whose real implementations are still ahead.
    //
    // This is not a choice. A head unit that advertises only video, input and audio is
    // one Android Auto refuses to project to at all: it reads the response, says
    // nothing, and drops out of accessory mode a second later. Offering the microphone
    // and the sensors as well is what makes it open the channels and start encoding.
    //
    // Which sensors are on that channel is the one part of this the host app chooses,
    // because it is the only thing that knows what the car has. See AaConfig::sensors.
    description.enable_video = true;
    description.enable_input = true;
    description.sensors = config.sensors;
    description.metadata = config.metadata;
    description.enable_media_audio = true;
    description.enable_system_audio = true;
    description.enable_speech_audio = true;
    description.enable_microphone = true;
    ApplyServiceOverride(&description);
    return description;
  }

  // The live connection, as a reference of the caller's own. See protocol_mutex.
  std::shared_ptr<aa::ProtocolSession> Protocol() const {
    std::lock_guard<std::mutex> lock(protocol_mutex);
    return protocol;
  }

  Config config;
  aa::EventBus events;
  aa::FrameRing ring;
  std::unique_ptr<aa::GlAdapter> gl;
  std::unique_ptr<aa::TestPattern> pattern;
  // Built once and kept across connections: opening VA-API costs tens of milliseconds
  // and a phone that comes and goes should not pay it every time.
  std::shared_ptr<aa::VideoDecoder> decoder;
  // The same reasoning, and one more: volume, mute and the chosen output describe the
  // head unit rather than the phone, so they have to survive a reconnect.
  std::shared_ptr<aa::AudioOutput> audio;
  // The microphone, kept across connections for the same reason the output is: which
  // input this head unit listens through is not the phone's business. It holds no device
  // open until a phone asks for one.
  std::shared_ptr<aa::AudioInput> microphone;
  // What the car is doing, which is the host app's to say and outlives every connection
  // for a plainer reason than the others: a parking brake does not come off because a
  // cable was pulled out.
  std::shared_ptr<aa::SensorState> sensors;
  // What the phone has said about itself. Created once so the host app can keep one
  // listener across reconnects, but emptied whenever a connection ends: a track that was
  // playing over a cable that has been pulled is not paused, it is gone.
  std::shared_ptr<aa::MetadataState> metadata;
  // The Dart side's raw PCM tap, or null. Written from the platform thread and read
  // from the audio writer threads, hence atomic.
  std::atomic<AaAudioCallback> audio_callback{nullptr};
  // The Dart side's metadata listener, on the same terms, read from io threads.
  std::atomic<AaMetadataCallback> metadata_callback{nullptr};

  std::unique_ptr<aa::UsbConnector> usb;
  // Wireless, or null until the host app asks for it. Unlike the USB connector this
  // one can be destroyed: it registers no callback with a library that outlives it,
  // and its Bluetooth thread is joined by Stop before anything is released.
  std::unique_ptr<aa::WirelessConnector> wireless;
  // What to tell a phone about the Wi-Fi network. Written from Flutter's platform
  // thread before wireless starts and read once when it does, which is why it needs
  // no lock: there is no moment at which both happen.
  aa::WirelessSettings wireless_settings;
  // The live connection, or nullptr between them.
  //
  // Assigned from an io_context thread when a phone reaches accessory mode, and read
  // from Flutter's platform thread by stop, so a quick start then stop has one thread
  // writing the pointer while the other is copying it. Reach it through Protocol(),
  // which hands back a reference of your own.
  mutable std::mutex protocol_mutex;
  std::shared_ptr<aa::ProtocolSession> protocol;
  // The input channel of whichever connection is live, published by the protocol
  // session as it comes and goes. Weak, so a stale entry cannot keep a finished
  // connection's messenger alive, and behind a mutex because it is written from an
  // io_context thread and read from Flutter's platform thread on every touch.
  std::mutex input_mutex;
  std::weak_ptr<aa::InputChannel> input;
  // The metadata channels of whichever connection is live, on the same terms as the
  // input channel and for the same reason: aa_session_browse arrives on Flutter's
  // platform thread while the session is built and torn down on io threads.
  std::mutex metadata_mutex;
  std::weak_ptr<aa::MetadataChannels> metadata_channels;

  // Recovery from a phone left wedged by a previous run. Bounded, so a genuinely broken
  // phone reports an error instead of looping forever.
  //
  // These three are written from io_context threads, in the state handler, and read
  // from the platform thread by start and stop.
  std::atomic<int> recovery_attempts{0};
  std::atomic<bool> reached_connected{false};
  // Whether a bounce is already in flight, see the note where it is set. Without it one
  // dead transport bounces the phone once per channel.
  std::atomic<bool> recovering{false};
  // Outlives every protocol session built on it, see the note on ProtocolSession::Create.
  std::unique_ptr<aasdk::Strand> channel_strand;

  // aasdk runs everything on this. Two threads is enough for the transport plus the
  // channel dispatch, and keeping it small makes the ordering easier to reason about.
  // It must never run on Flutter's platform thread.
  boost::asio::io_context io_context;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard;
  std::vector<std::thread> io_threads;
  std::atomic<bool> running{false};
};

namespace {

// The live input channel, or nullptr when no phone is connected. Called on Flutter's
// platform thread.
std::shared_ptr<aa::InputChannel> LockInput(AaSession* session) {
  if (session == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(session->input_mutex);
  return session->input.lock();
}

// The live metadata channels, or nullptr when no phone is connected. Called on
// Flutter's platform thread.
std::shared_ptr<aa::MetadataChannels> LockMetadata(AaSession* session) {
  if (session == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(session->metadata_mutex);
  return session->metadata_channels.lock();
}

// Builds the session for one connection and wires up everything that has to be told
// when it changes state.
//
// `wireless` picks the recovery, which is the one thing the two transports do not
// share. A cable that goes quiet leaves the phone enumerated and still in accessory
// mode, so it has to be bounced; a Wi-Fi link that goes quiet leaves nothing behind at
// all, so opening the port again and waiting is the whole of it.
std::shared_ptr<aa::ProtocolSession> NewProtocolSession(AaSession* session,
                                                        bool wireless) {
  if (auto previous = session->Protocol()) {
    previous->Stop();
  }
  session->reached_connected = false;

  auto protocol = aa::ProtocolSession::Create(
      session->io_context, *session->channel_strand, session->Describe(),
      session->decoder, session->audio, session->microphone, session->sensors,
      session->metadata,
      [session, wireless](int state, const std::string& message) {
        if (state == AA_STATE_CONNECTED) {
          session->reached_connected = true;
          session->recovery_attempts = 0;
        }

        // One dead transport is one event, however many channels notice it. They all
        // do, within a millisecond of each other, and the channels report their
        // failures as messages on the connected state, which puts reached_connected
        // back up between them. Without this guard the first error starts a bounce and
        // the next six each start another.
        if (state == AA_STATE_ERROR && session->recovering) {
          return;
        }

        if (state == AA_STATE_ERROR && wireless) {
          // Nothing to bounce and nothing to re-enumerate. Either the phone left the
          // network, in which case it will dial in again when it comes back, or the
          // link broke, in which case it will notice long before this end does. Both
          // want the same thing: the port open and somebody listening.
          session->reached_connected = false;
          session->recovery_attempts = 0;
          session->recovering = true;
          session->events.Emit(AA_STATE_SEARCHING,
                               "Lost the phone over Wi-Fi, waiting for it to come "
                               "back. " +
                                   message);
          if (session->wireless) {
            session->wireless->Rearm();
          }
          return;
        }

        // A session that was connected and then failed has lost the transport.
        // Sometimes that is the cable coming out, and the phone will re-enumerate on
        // its own when it goes back in. Often it is not: a single failed bulk transfer
        // (LIBUSB_TRANSFER_ERROR on a marginal link, say) kills the transport while
        // leaving the phone enumerated and still in accessory mode. Waiting for a
        // hotplug then waits forever, because the device never left, which is why
        // stopping and starting by hand was the only way out: stopping resets the
        // device and that is what re-arms it.
        //
        // So bounce the phone rather than waiting to be told about it. This is right
        // for both cases: if the cable really is out the reset fails harmlessly on a
        // device that has already gone, and re-arming discovery is what the replug
        // needs anyway.
        if (state == AA_STATE_ERROR && session->reached_connected) {
          session->reached_connected = false;
          session->recovery_attempts = 0;
          // The original error comes along. It is usually the transport, but the
          // description of what actually failed is the only thing that tells one
          // transport failure from another, and throwing it away made them all look
          // identical.
          session->events.Emit(
              AA_STATE_SEARCHING,
              "Lost the link to the phone, resetting it and reconnecting. " + message);
          if (session->usb) {
            session->recovering = true;
            session->usb->RecoverAndRediscover();
          }
          return;
        }

        // A session that fails before it ever connected usually means the phone is
        // still in accessory mode from a run that died without saying goodbye. It will
        // not answer on those endpoints again until it has been through the AOAP
        // handshake, so bounce it and start over rather than reporting a dead end the
        // user can only fix by unplugging the cable.
        if (state == AA_STATE_ERROR && !session->reached_connected &&
            session->recovery_attempts < kMaxRecoveryAttempts && session->usb) {
          ++session->recovery_attempts;
          session->events.Emit(
              AA_STATE_SEARCHING,
              "The phone did not answer, resetting it and trying again (" +
                  std::to_string(session->recovery_attempts) + " of " +
                  std::to_string(kMaxRecoveryAttempts) + ").");
          session->recovering = true;
          session->usb->RecoverAndRediscover();
          return;
        }
        session->events.Emit(static_cast<AaState>(state), message);
      },
      [session](std::shared_ptr<aa::InputChannel> input) {
        std::lock_guard<std::mutex> lock(session->input_mutex);
        session->input = input;
      },
      [session](std::shared_ptr<aa::MetadataChannels> metadata) {
        std::lock_guard<std::mutex> lock(session->metadata_mutex);
        session->metadata_channels = metadata;
      });
  {
    std::lock_guard<std::mutex> lock(session->protocol_mutex);
    session->protocol = protocol;
  }
  return protocol;
}

// Publishes the Bluetooth service and opens the projection port. Returns the same
// codes aa_session_start_wireless does, which is the only caller besides
// aa_session_start.
int32_t StartWirelessTransport(AaSession* session) {
  if (!session->running) {
    return -1;
  }
  if (!session->wireless) {
    session->wireless = std::make_unique<aa::WirelessConnector>(session->io_context);
  }
  if (session->wireless->running()) {
    aa::WirelessSettings wanted = session->wireless_settings;
    ApplyWirelessOverride(&wanted);
    if (session->wireless->settings() == wanted) {
      return 0;
    }
    // The host app changed the network since this started. Restart rather than
    // quietly keeping the old one: a driver who types a passphrase and presses start
    // has every right to expect that passphrase to be the one offered.
    //
    // Safe mid journey. Stop only withdraws the offer, and a phone that is already
    // projecting holds its own transport, so this does not touch it.
    session->wireless->Stop();
  }
  aa::WirelessSettings settings = session->wireless_settings;
  ApplyWirelessOverride(&settings);
  const std::string error = session->wireless->Start(
      settings,
      [session](aasdk::transport::ITransport::Pointer transport, std::string peer) {
        // A cable beats a radio, and a connection that is already working beats one
        // that is merely offered. A phone projecting over USB while its Wi-Fi side
        // dials in would otherwise have the wired session torn down under it, which
        // looks to the driver like the screen going black for no reason.
        //
        // Not symmetric, deliberately. A cable being plugged in is something a person
        // did on purpose, so the USB path is allowed to take over; a wireless
        // connection arriving is not, so it waits its turn.
        if (session->reached_connected) {
          AASDK_LOG(info) << "[Wireless] " << peer
                          << " dialled in while a session is already connected, "
                             "leaving the connected one alone";
          transport->stop();
          if (session->wireless) {
            session->wireless->Rearm();
          }
          return;
        }
        // A phone dialled in, so whatever wait was in flight is over.
        session->recovering = false;
        auto protocol = NewProtocolSession(session, true);
        session->events.Emit(AA_STATE_HANDSHAKING,
                             "Phone connected over Wi-Fi from " + peer + ".");
        protocol->Start(std::move(transport));
      },
      [session](int state, const std::string& message) {
        // Bluetooth and Wi-Fi progress is news rather than a lifecycle change, in the
        // same way the decoder's is. Searching is the truthful state while nothing is
        // connected, so it goes out as that, but it must never move a connected
        // session backwards: a second phone touching Bluetooth while the first one is
        // projecting had the head unit reporting that it was looking for a phone, and
        // every report after that, the video statistics included, inherited it.
        //
        // Errors are a different matter and go out as themselves. Nothing here can
        // recover a session, so there is no bounce to debounce.
        if (state == AA_STATE_SEARCHING && session->reached_connected) {
          session->events.Emit(session->events.last_state(), message);
          return;
        }
        session->events.Emit(static_cast<AaState>(state), message);
      });
  if (!error.empty()) {
    session->events.Emit(AA_STATE_ERROR, error);
    return -2;
  }
  return 0;
}

// Every sensor setter has the same guard in front of it.
aa::SensorState* Sensors(AaSession* session) {
  if (session == nullptr || !session->sensors) {
    return nullptr;
  }
  return session->sensors.get();
}

}  // namespace

extern "C" {

void aa_string_free(char* message) { free(message); }

void aa_audio_buffer_free(uint8_t* data) { free(data); }

char* aa_audio_devices(void) {
  std::string listing;
  for (const aa::PcmDevice& device : aa::ListPcmDevices()) {
    listing += device.name;
    listing += '\t';
    listing += device.description;
    listing += '\t';
    listing += device.is_default ? '1' : '0';
    listing += '\n';
  }
  return strdup(listing.c_str());
}

char* aa_microphone_devices(void) {
  std::string listing;
  for (const aa::PcmDevice& device : aa::ListPcmCaptureDevices()) {
    listing += device.name;
    listing += '\t';
    listing += device.description;
    listing += '\t';
    listing += device.is_default ? '1' : '0';
    listing += '\n';
  }
  return strdup(listing.c_str());
}

AaSession* aa_session_create(const AaConfig* config, AaEventCallback on_event) {
  ApplyAasdkLogLevel();
  auto* session = new AaSession(on_event);
  if (config != nullptr) {
    session->config.width = config->width > 0 ? config->width : 1280;
    session->config.height = config->height > 0 ? config->height : 720;
    session->config.fps = config->fps > 0 ? config->fps : 30;
    session->config.dpi = config->dpi > 0 ? config->dpi : 140;
    session->config.head_unit_name = CopyOrEmpty(config->head_unit_name);
    session->config.car_model = CopyOrEmpty(config->car_model);
    session->config.car_year = CopyOrEmpty(config->car_year);
    // Zero is a host app that did not ask, not one that wants no sensors at all. A head
    // unit with no sensor channel is one Android Auto will not finish opening its
    // interface for, so the two that are not optional are the floor.
    session->config.sensors = config->sensors == 0
                                  ? aa::kRequiredSensors
                                  : static_cast<aa::SensorMask>(config->sensors);
    // Negative is a host app that did not ask, so it gets the three the phone pushes on
    // its own. Zero is a host app that wants none of them, which is a real choice and
    // not the same thing: unlike the sensors, there is no metadata channel a phone
    // insists on.
    session->config.metadata = config->metadata < 0
                                   ? aa::kDefaultMetadata
                                   : static_cast<aa::MetadataMask>(config->metadata);
    // Zero is a host app written before wireless existed, and a head unit that stops
    // answering the cable because of a recompile would be a nasty surprise.
    session->config.transports =
        config->transports == 0 ? AA_TRANSPORT_USB : config->transports;
  }

  session->ring.Configure(session->config.width, session->config.height);
  session->gl = std::make_unique<aa::GlAdapter>(&session->ring);
  session->pattern = std::make_unique<aa::TestPattern>(
      &session->ring, [session]() { session->gl->NotifyFrameAvailable(); });

  session->decoder = std::make_shared<aa::VideoDecoder>(
      [session](const aa::Frame& frame, std::shared_ptr<void> keepalive) {
        // Real video supersedes the test pattern rather than fighting it for slots.
        if (session->pattern->running()) {
          session->pattern->Stop();
        }
        if (session->ring.PublishFrame(frame, std::move(keepalive))) {
          session->gl->NotifyFrameAvailable();
        }
      },
      [session](const std::string& message) {
        // Video news is not a lifecycle change, so it keeps whatever state the session
        // is already in rather than inventing one.
        session->events.Emit(session->events.last_state(), message);
      });
  // The present adapter is the only thing that knows whether a dmabuf can be imported,
  // and it only knows once Flutter has drawn once. Asking it lazily, per open, is what
  // lets a machine without the EGL extension end up on software decode by itself.
  session->decoder->SetDmabufProbe([]() { return aa::GlAdapter::DmabufSupported(); });

  session->audio = std::make_shared<aa::AudioOutput>([session](const std::string& message) {
    // Audio news is not a lifecycle change, so it keeps whatever state the session is
    // already in rather than inventing one, exactly as the decoder's does.
    session->events.Emit(session->events.last_state(), message);
  });
  session->microphone =
      std::make_shared<aa::AudioInput>([session](const std::string& message) {
        session->events.Emit(session->events.last_state(), message);
      });
  session->sensors = std::make_shared<aa::SensorState>();
  session->metadata = std::make_shared<aa::MetadataState>();
  return session;
}

void aa_session_destroy(AaSession* session) {
  if (session == nullptr) {
    return;
  }
  aa_session_stop(session);
  // Close the bus before dropping anything else, so a straggling event cannot reach a
  // NativeCallable the Dart side is about to tear down.
  session->events.Close();
  if (session->decoder) {
    session->decoder->Stop();
  }
  if (session->audio) {
    // Same reasoning as closing the bus: a buffer in flight must not reach a
    // NativeCallable the Dart side is about to tear down.
    session->audio->SetTap(nullptr);
    session->audio->Stop();
  }
  if (session->microphone) {
    // Nothing here reaches Dart, but a capture thread outliving the session would hold a
    // handler pointing into a channel that is going away.
    session->microphone->Stop();
  }
  session->audio_callback = nullptr;
  session->metadata_callback = nullptr;
  if (session->metadata) {
    // Same reasoning as closing the bus: an update in flight must not reach a
    // NativeCallable the Dart side is about to tear down.
    session->metadata->SetListener(nullptr);
  }
  session->gl.reset();
  if (session->usb) {
    // The connector outlives this session, so cut its link back to it first.
    session->usb->ClearHandlers();
  }
  if (session->wireless) {
    // Destroyed rather than leaked, unlike the USB connector below: nothing in BlueZ
    // or asio holds a callback into it once Stop has withdrawn the profile, joined the
    // Bluetooth thread and closed the acceptor, and aa_session_stop above has already
    // drained the io_context that the accept handler would have run on.
    session->wireless->ClearHandlers();
    session->wireless->Stop();
    session->wireless.reset();
  }
  // Deliberately not destroyed. USBHub registers a libusb hotplug callback that holds a
  // raw pointer back to itself and calls shared_from_this() when a device arrives; if
  // the hub is gone by then, that throws std::bad_weak_ptr from inside libusb's event
  // thread and terminates the process. Deregistration is asynchronous, so there is no
  // moment at which destroying it is provably safe.
  //
  // libusb is already process wide for the same reason (see usb_context.h), so one
  // connector outliving its session costs a few kilobytes and removes the last crash of
  // this kind. A process has one USB bus and one head unit.
  session->usb.release();
  // Same reasoning: aasdk's Channel holds this by reference and posts to it from
  // handlers that outlive the session that created them.
  session->channel_strand.release();
  delete session;
}

int32_t aa_session_start(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if (session->running) {
    return 0;
  }

  // Register the texture here rather than lazily on the producer thread: this call
  // comes from Dart, which means the platform thread, and that is where Flutter wants
  // texture registration to happen.
  if (!session->gl->Register()) {
    session->events.Emit(AA_STATE_ERROR,
                         "Could not register a Flutter texture. The plugin's GTK entry "
                         "point did not run.");
    return -2;
  }

  session->work_guard = std::make_unique<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
      boost::asio::make_work_guard(session->io_context));

  const unsigned int thread_count = 2;
  for (unsigned int i = 0; i < thread_count; ++i) {
    session->io_threads.emplace_back([session]() {
      // aasdk throws aasdk::error::Error out of its handlers for things like a USB
      // interface that will not claim. An exception escaping io_context::run() on a
      // plain std::thread calls std::terminate and takes the whole app with it, which
      // is not an acceptable response to a phone being unplugged at a bad moment.
      for (;;) {
        try {
          session->io_context.run();
          return;
        } catch (const aasdk::error::Error& error) {
          session->events.Emit(AA_STATE_ERROR,
                               std::string("Transport error: ") + error.what());
        } catch (const std::exception& error) {
          session->events.Emit(AA_STATE_ERROR,
                               std::string("Unexpected native error: ") + error.what());
        }
      }
    });
  }
  session->running = true;
  session->recovering = false;
  session->decoder->Start();
  session->audio->Start();

  if (!session->usb) {
    session->usb = std::make_unique<aa::UsbConnector>(session->io_context);
  }
  if (!session->channel_strand) {
    session->channel_strand = std::make_unique<aasdk::Strand>(session->io_context);
  }
  const int32_t transports = ApplyTransportOverride(session->config.transports);
  const bool want_usb = (transports & AA_TRANSPORT_USB) != 0;
  const bool want_wireless = (transports & AA_TRANSPORT_WIRELESS) != 0;

  if (want_usb) {
    const std::string usb_error = session->usb->Start(
        [session](aasdk::usb::IAOAPDevice::Pointer device) {
          // A phone reached accessory mode. Hand it to a fresh protocol session; any
          // previous one belongs to a connection that has already gone away.
          //
          // A device arrived, so whatever bounce was in flight has done its job.
          session->recovering = false;
          auto protocol = NewProtocolSession(session, false);
          // Started from the local reference: a stop landing right now can take the
          // member away, and Start() itself waits for that stop rather than racing it.
          protocol->Start(std::move(device));
        },
        [session](const std::string& message) {
          session->events.Emit(AA_STATE_ERROR, message);
        });
    if (!usb_error.empty()) {
      session->events.Emit(AA_STATE_ERROR, usb_error);
      return -3;
    }
  }

  if (want_wireless) {
    // An error here has already been reported, and it must not stop a head unit that
    // also has a cable: a machine with no Bluetooth is still a working wired head
    // unit, and failing the whole start would make it neither.
    StartWirelessTransport(session);
  }

  session->events.Emit(AA_STATE_SEARCHING, want_usb && want_wireless
                                               ? "Looking for a phone on USB or Wi-Fi."
                                           : want_wireless
                                               ? "Looking for a phone over Wi-Fi."
                                               : "Looking for a phone on USB.");
  return 0;
}

int32_t aa_session_stop(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if (session->pattern) {
    session->pattern->Stop();
  }
  if (session->decoder) {
    session->decoder->Stop();
  }
  if (session->audio) {
    // Before the protocol teardown, so the speakers go quiet when the user presses stop
    // rather than a buffer later.
    session->audio->Stop();
  }
  if (session->microphone) {
    // Before the protocol teardown as well, and for a stronger reason than the speakers:
    // stop means stop listening, now, not once the goodbye has been acknowledged.
    session->microphone->Stop();
  }
  // Ask, but do not yet destroy. Both of these only queue cancellations onto the
  // io_context, and those queued handlers still need libusb and the USB device to be
  // alive when they run.
  if (auto protocol = session->Protocol()) {
    // Say goodbye and actually wait to be heard. A fixed sleep was not enough: the
    // phone keeps Android Auto running, and its claim on the USB interface, until it
    // acknowledges. Waiting for the acknowledgement is what makes resuming work.
    //
    // Shutdown() blocks until a connection still being built on an io thread has
    // finished, so that a stop pressed a moment after a start still says goodbye on a
    // real control channel instead of finding none and leaving Android Auto running on
    // the phone.
    protocol->Shutdown();
    const bool acknowledged =
        protocol->WaitForShutdown(std::chrono::milliseconds(1500));
    if (!acknowledged) {
      session->events.Emit(
          AA_STATE_IDLE,
          "The phone did not acknowledge the shutdown. It may keep Android Auto running, "
          "which can delay the next connection.");
    }
    protocol->Stop();
  }
  if (session->usb) {
    session->usb->Stop();
    // Bounce the phone out of accessory mode so a later start redoes the AOAP handshake
    // from scratch. Without this the phone stays in accessory mode with its Android Auto
    // session closed, and the next version request simply times out.
    session->usb->ResetDevice();
  }
  if (session->wireless) {
    // Declined rather than withdrawn. A phone that knows this machine as a wireless
    // car asks for the service every five seconds for as long as Bluetooth is
    // connected, and withdrawing it does not stop the asking, it only stops the
    // answering: the driver is left with a notification saying the phone is
    // connecting, permanently, while nothing is. Refusing makes it give up, and a
    // head unit whose session the user has stopped is exactly a head unit that should
    // be saying no. The service goes away for good in aa_session_destroy.
    session->wireless->Decline();
  }
  if (!session->running) {
    std::lock_guard<std::mutex> lock(session->protocol_mutex);
    session->protocol.reset();
    return 0;
  }

  // Drain rather than abandon. Dropping the work guard lets run() return once the
  // queued handlers are done, and those handlers hold the last references to the
  // session objects. Calling io_context::stop() here instead would leave them queued
  // and undestroyed, so the USB interface would still be claimed on the next start and
  // the reconnect would fail with LIBUSB_ERROR_BUSY.
  session->work_guard.reset();

  // The drain must not be able to hang the caller, which is Flutter's platform thread.
  // A watchdog stops the io_context hard if the queued handlers have not finished in
  // time, at the cost of the clean release this is trying to achieve.
  std::atomic<bool> joined{false};
  std::thread watchdog([&session, &joined]() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!joined.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!joined.load()) {
      session->io_context.stop();
    }
  });

  for (auto& thread : session->io_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  joined = true;
  watchdog.join();

  session->io_threads.clear();

  // The protocol session is rebuilt on every connection, so drop it. The USB connector
  // is not: see the note on UsbConnector::Start about why its aasdk objects have to
  // outlive the hotplug callback libusb holds.
  {
    std::lock_guard<std::mutex> lock(session->protocol_mutex);
    session->protocol.reset();
  }

  session->io_context.restart();
  session->running = false;
  session->recovering = false;
  // Nothing is left to draw, and the last frame is holding a dmabuf open. Let go of it.
  session->ring.Reset();

  session->events.Emit(AA_STATE_IDLE);
  return 0;
}

int64_t aa_session_texture_id(AaSession* session) {
  if (session == nullptr || !session->gl) {
    return -1;
  }
  return session->gl->texture_id();
}

int32_t aa_session_video_width(AaSession* session) {
  if (session == nullptr || !session->decoder) {
    return 0;
  }
  return session->decoder->frame_width();
}

int32_t aa_session_video_height(AaSession* session) {
  if (session == nullptr || !session->decoder) {
    return 0;
  }
  return session->decoder->frame_height();
}

char* aa_session_video_backend(AaSession* session) {
  const std::string name =
      session == nullptr || !session->decoder ? std::string("none")
                                              : session->decoder->backend_name();
  return strdup(name.c_str());
}

int32_t aa_session_send_touch(AaSession* session, int32_t action, int32_t action_index,
                              const AaTouchPoint* points, int32_t count) {
  if (points == nullptr || count <= 0) {
    return -1;
  }
  auto input = LockInput(session);
  if (!input) {
    return -2;
  }
  std::vector<aa::TouchPoint> fingers;
  fingers.reserve(static_cast<size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    fingers.push_back({points[i].id, points[i].x, points[i].y});
  }
  input->SendTouch(static_cast<aa::TouchAction>(action), action_index,
                   std::move(fingers));
  return 0;
}

int32_t aa_session_send_key(AaSession* session, int32_t keycode, int32_t down,
                            int32_t long_press) {
  auto input = LockInput(session);
  if (!input) {
    return -2;
  }
  input->SendKey(keycode, down != 0, long_press != 0);
  return 0;
}

int32_t aa_session_send_rotary(AaSession* session, int32_t steps) {
  auto input = LockInput(session);
  if (!input) {
    return -2;
  }
  input->SendRotary(steps);
  return 0;
}

int32_t aa_session_set_audio_volume(AaSession* session, int32_t stream,
                                    double volume) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return -1;
  }
  session->audio->SetVolume(which, volume);
  return 0;
}

double aa_session_audio_volume(AaSession* session, int32_t stream) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return 0.0;
  }
  return session->audio->Volume(which);
}

int32_t aa_session_set_audio_muted(AaSession* session, int32_t stream, int32_t muted) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return -1;
  }
  session->audio->SetMuted(which, muted != 0);
  return 0;
}

int32_t aa_session_audio_muted(AaSession* session, int32_t stream) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return 0;
  }
  return session->audio->Muted(which) ? 1 : 0;
}

int32_t aa_session_set_audio_device(AaSession* session, const char* device) {
  if (session == nullptr || !session->audio) {
    return -1;
  }
  session->audio->SetDevice(CopyOrEmpty(device));
  return 0;
}

char* aa_session_audio_device(AaSession* session) {
  const std::string device =
      session == nullptr || !session->audio ? std::string() : session->audio->device();
  return strdup(device.c_str());
}

int32_t aa_session_set_audio_output_enabled(AaSession* session, int32_t enabled) {
  if (session == nullptr || !session->audio) {
    return -1;
  }
  session->audio->SetOutputEnabled(enabled != 0);
  return 0;
}

int32_t aa_session_audio_output_enabled(AaSession* session) {
  if (session == nullptr || !session->audio) {
    return 0;
  }
  return session->audio->output_enabled() ? 1 : 0;
}

int32_t aa_session_set_audio_callback(AaSession* session, AaAudioCallback on_audio) {
  if (session == nullptr || !session->audio) {
    return -1;
  }
  session->audio_callback = on_audio;
  if (on_audio == nullptr) {
    session->audio->SetTap(nullptr);
    return 0;
  }
  session->audio->SetTap([session](aa::AudioStream stream, const aa::PcmFormat& format,
                                   const uint8_t* data, size_t size) {
    // Read once. The Dart side can clear the tap from the platform thread while this
    // runs, and a second load could see the null.
    AaAudioCallback callback = session->audio_callback.load();
    if (callback == nullptr || size == 0) {
      return;
    }
    // Copied onto the heap because the callback is a NativeCallable.listener: the call
    // is delivered to the isolate after this returns, so a pointer into the writer
    // thread's buffer would already be gone by the time Dart read it. Ownership passes
    // to Dart, which hands it back to aa_audio_buffer_free.
    auto* copy = static_cast<uint8_t*>(malloc(size));
    if (copy == nullptr) {
      return;
    }
    memcpy(copy, data, size);
    callback(static_cast<int32_t>(stream), copy, static_cast<int32_t>(size),
             format.sample_rate, format.channels);
  });
  return 0;
}

char* aa_session_audio_backend(AaSession* session) {
  const std::string name = session == nullptr || !session->audio
                               ? std::string("none")
                               : session->audio->backend_name();
  return strdup(name.c_str());
}

int64_t aa_session_audio_underruns(AaSession* session, int32_t stream) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return 0;
  }
  return static_cast<int64_t>(session->audio->Underruns(which));
}

int64_t aa_session_audio_dropped(AaSession* session, int32_t stream) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return 0;
  }
  return static_cast<int64_t>(session->audio->Dropped(which));
}

int64_t aa_session_audio_latency(AaSession* session, int32_t stream) {
  aa::AudioStream which;
  if (session == nullptr || !session->audio || !ToAudioStream(stream, &which)) {
    return 0;
  }
  return session->audio->LatencyMicros(which);
}

int32_t aa_session_microphone_active(AaSession* session) {
  if (session == nullptr || !session->microphone) {
    return 0;
  }
  return session->microphone->active() ? 1 : 0;
}

double aa_session_microphone_level(AaSession* session) {
  if (session == nullptr || !session->microphone) {
    return 0.0;
  }
  return session->microphone->level();
}

int64_t aa_session_microphone_bytes(AaSession* session) {
  if (session == nullptr || !session->microphone) {
    return 0;
  }
  return static_cast<int64_t>(session->microphone->bytes_captured());
}

int32_t aa_session_set_microphone_device(AaSession* session, const char* device) {
  if (session == nullptr || !session->microphone) {
    return -1;
  }
  session->microphone->SetDevice(CopyOrEmpty(device));
  return 0;
}

char* aa_session_microphone_device(AaSession* session) {
  const std::string device = session == nullptr || !session->microphone
                                 ? std::string()
                                 : session->microphone->device();
  return strdup(device.c_str());
}

char* aa_session_microphone_backend(AaSession* session) {
  const std::string name = session == nullptr || !session->microphone
                               ? std::string("none")
                               : session->microphone->backend_name();
  return strdup(name.c_str());
}

int32_t aa_session_set_night_mode(AaSession* session, int32_t night) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetNightMode(night != 0);
  return 0;
}

int32_t aa_session_set_driving_status(AaSession* session, int32_t restrictions) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetDrivingRestrictions(restrictions);
  return 0;
}

int32_t aa_session_set_location(AaSession* session, const AaLocation* location) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr || location == nullptr) {
    return -1;
  }
  aa::Location fix;
  fix.latitude = location->latitude;
  fix.longitude = location->longitude;
  fix.accuracy_metres = location->accuracy_metres;
  fix.altitude_metres = location->altitude_metres;
  fix.speed_mps = location->speed_mps;
  fix.bearing_degrees = location->bearing_degrees;
  sensors->SetLocation(fix);
  return 0;
}

int32_t aa_session_set_speed(AaSession* session, double metres_per_second) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetSpeed(metres_per_second);
  return 0;
}

int32_t aa_session_set_rpm(AaSession* session, double rpm) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetRpm(rpm);
  return 0;
}

int32_t aa_session_set_fuel(AaSession* session, double level_percent,
                            double range_metres, int32_t low) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetFuel(level_percent, range_metres, low != 0);
  return 0;
}

int32_t aa_session_set_parking_brake(AaSession* session, int32_t engaged) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetParkingBrake(engaged != 0);
  return 0;
}

int32_t aa_session_set_gear(AaSession* session, int32_t gear) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetGear(gear);
  return 0;
}

int32_t aa_session_set_compass(AaSession* session, double bearing_degrees) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetCompass(bearing_degrees);
  return 0;
}

int32_t aa_session_set_environment(AaSession* session, double temperature_celsius,
                                   double pressure_kpa) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetEnvironment(temperature_celsius, pressure_kpa);
  return 0;
}

int32_t aa_session_set_odometer(AaSession* session, double kilometres) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetOdometer(kilometres);
  return 0;
}

int32_t aa_session_set_toll_card(AaSession* session, int32_t present) {
  auto* sensors = Sensors(session);
  if (sensors == nullptr) {
    return -1;
  }
  sensors->SetTollCard(present != 0);
  return 0;
}

int32_t aa_session_sensor_subscriptions(AaSession* session) {
  auto* sensors = Sensors(session);
  return sensors == nullptr ? 0 : static_cast<int32_t>(sensors->subscriptions());
}

int64_t aa_session_sensor_batches(AaSession* session) {
  auto* sensors = Sensors(session);
  return sensors == nullptr ? 0 : static_cast<int64_t>(sensors->batches_sent());
}

int32_t aa_session_set_metadata_callback(AaSession* session,
                                         AaMetadataCallback on_metadata) {
  if (session == nullptr || !session->metadata) {
    return -1;
  }
  session->metadata_callback = on_metadata;
  if (on_metadata == nullptr) {
    session->metadata->SetListener(nullptr);
    return 0;
  }
  session->metadata->SetListener(
      [session](aa::Metadata which, const std::string& json) {
        // Read once. The Dart side can clear the listener from the platform thread
        // while this runs, and a second load could see the null.
        AaMetadataCallback callback = session->metadata_callback.load();
        if (callback == nullptr) {
          return;
        }
        // strdup rather than passing c_str(): the callback is a
        // NativeCallable.listener, so the call is delivered to the isolate after this
        // returns and the string is long gone by then. Ownership passes to Dart, which
        // hands it back to aa_string_free.
        callback(static_cast<int32_t>(which), strdup(json.c_str()));
      });
  return 0;
}

char* aa_session_metadata(AaSession* session, int32_t kind) {
  aa::Metadata which;
  if (session == nullptr || !session->metadata || !ToMetadata(kind, &which)) {
    return strdup("");
  }
  return strdup(session->metadata->Snapshot(which).c_str());
}

int32_t aa_session_metadata_channels(AaSession* session) {
  if (session == nullptr || !session->metadata) {
    return 0;
  }
  return static_cast<int32_t>(session->metadata->opened());
}

int64_t aa_session_metadata_updates(AaSession* session, int32_t kind) {
  aa::Metadata which;
  if (session == nullptr || !session->metadata || !ToMetadata(kind, &which)) {
    return 0;
  }
  return session->metadata->updates(which);
}

int32_t aa_session_browse(AaSession* session, const char* path, int32_t start) {
  auto metadata = LockMetadata(session);
  if (!metadata) {
    return -2;
  }
  return metadata->Browse(CopyOrEmpty(path), start) ? 0 : -2;
}

int32_t aa_session_browse_select(AaSession* session, const char* path) {
  auto metadata = LockMetadata(session);
  if (!metadata) {
    return -2;
  }
  return metadata->BrowseSelect(CopyOrEmpty(path)) ? 0 : -2;
}

char* aa_paired_phones(void) {
  std::string error;
  std::string listing;
  for (const aa::BluetoothDevice& device : aa::BluezClient::PairedDevices(&error)) {
    listing += device.address;
    listing += '\t';
    listing += device.name;
    listing += '\t';
    listing += device.connected ? '1' : '0';
    listing += '\t';
    listing += device.is_phone ? '1' : '0';
    listing += '\n';
  }
  return strdup(listing.c_str());
}

int32_t aa_session_set_wireless_config(AaSession* session,
                                       const AaWirelessConfig* config) {
  if (session == nullptr || config == nullptr) {
    return -1;
  }
  aa::WirelessSettings settings;
  settings.ssid = CopyOrEmpty(config->ssid);
  settings.passphrase = CopyOrEmpty(config->passphrase);
  settings.bssid = CopyOrEmpty(config->bssid);
  settings.interface = CopyOrEmpty(config->interface_name);
  settings.ip = CopyOrEmpty(config->ip_address);
  settings.phone_address = CopyOrEmpty(config->phone_address);
  settings.port = config->port > 0 && config->port <= 65535
                      ? static_cast<uint16_t>(config->port)
                      : 5288;
  settings.security = config->security == 0 ? AA_WIFI_WPA2_PERSONAL : config->security;
  settings.access_point = config->access_point;
  session->wireless_settings = settings;
  return 0;
}

int32_t aa_session_start_wireless(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  return StartWirelessTransport(session);
}

int32_t aa_session_stop_wireless(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if (session->wireless) {
    // Only the offer is withdrawn. A phone already projecting keeps its connection,
    // because a driver who turns wireless off halfway through a journey meant "do not
    // start another one", not "cut this one off in a tunnel".
    //
    // And the Bluetooth service stays published so that a phone asking for it is told
    // no rather than ignored, which is what actually stops it asking. See
    // WirelessConnector::Decline.
    session->wireless->Decline();
  }
  return 0;
}

int32_t aa_session_decline_wireless(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if ((session->config.transports & AA_TRANSPORT_WIRELESS) == 0) {
    // A head unit that does not do wireless must not publish the service, even to
    // refuse on it. A phone reads the service list at pairing time and decides from
    // it that a machine is a wireless car; publishing anything here would teach a
    // phone paired today to start asking tomorrow.
    return 0;
  }
  if (!session->wireless) {
    session->wireless = std::make_unique<aa::WirelessConnector>(session->io_context);
  }
  session->wireless->Decline();
  return 0;
}

int32_t aa_session_wireless_active(AaSession* session) {
  if (session == nullptr || !session->wireless) {
    return 0;
  }
  return session->wireless->running() ? 1 : 0;
}

char* aa_session_wireless_summary(AaSession* session) {
  if (session == nullptr || !session->wireless || !session->wireless->running()) {
    return strdup("");
  }
  const aa::WirelessSummary summary = session->wireless->summary();
  std::string line;
  line += summary.interface;
  line += '\t';
  line += summary.ssid;
  line += '\t';
  line += summary.bssid;
  line += '\t';
  line += summary.ip;
  line += '\t';
  line += std::to_string(summary.port);
  line += '\t';
  line += summary.hosting ? '1' : '0';
  line += '\t';
  line += summary.bluetooth_ready ? '1' : '0';
  line += '\t';
  line += summary.phone_linked ? '1' : '0';
  return strdup(line.c_str());
}

int32_t aa_session_start_test_pattern(AaSession* session) {
  if (session == nullptr || !session->pattern) {
    return -1;
  }
  if (!session->gl->Register()) {
    return -2;
  }
  session->pattern->Start(session->config.width, session->config.height,
                          session->config.fps);
  return 0;
}

int32_t aa_session_stop_test_pattern(AaSession* session) {
  if (session == nullptr || !session->pattern) {
    return -1;
  }
  session->pattern->Stop();
  return 0;
}

}  // extern "C"
