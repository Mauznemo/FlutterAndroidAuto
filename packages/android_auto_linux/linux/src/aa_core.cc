#include "aa_core.h"

#include <boost/asio.hpp>

#include <aasdk/Common/Strand.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "event_bus.h"
#include "frame_ring.h"
#include "present/gl_adapter.h"
#include "session/protocol_session.h"
#include "session/service_discovery.h"
#include "session/usb_connector.h"
#include "test_pattern.h"

namespace {

// How many times to bounce an unresponsive phone before giving up and telling the user.
constexpr int kMaxRecoveryAttempts = 3;

std::string CopyOrEmpty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

// A head unit session.
//
// Owns the io_context thread pool that aasdk will run on from M3, the video frame path,
// and the channel back to Dart. Nothing here touches Flutter's platform thread except
// the FFI entry points themselves, which Dart already calls from it.
struct AaSession {
  struct Config {
    int32_t width = 1280;
    int32_t height = 720;
    int32_t fps = 30;
    int32_t dpi = 140;
    std::string head_unit_name;
    std::string car_model;
    std::string car_year;
    std::string certificate_path;
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
    // Only video, input and sensors are advertised so far. Audio goes on in M6, the
    // microphone in M7. Advertising a channel with no handler gets the connection
    // dropped, so these stay off until their milestone lands.
    description.enable_video = true;
    description.enable_input = true;
    description.enable_sensors = true;
    description.enable_media_audio = false;
    description.enable_system_audio = false;
    description.enable_speech_audio = false;
    description.enable_microphone = false;
    return description;
  }

  Config config;
  aa::EventBus events;
  aa::FrameRing ring;
  std::unique_ptr<aa::GlAdapter> gl;
  std::unique_ptr<aa::TestPattern> pattern;

  std::unique_ptr<aa::UsbConnector> usb;
  std::shared_ptr<aa::ProtocolSession> protocol;
  // Recovery from a phone left wedged by a previous run. Bounded, so a genuinely broken
  // phone reports an error instead of looping forever.
  int recovery_attempts = 0;
  bool reached_connected = false;
  // Outlives every protocol session built on it, see the note on ProtocolSession::Create.
  std::unique_ptr<aasdk::Strand> channel_strand;

  // aasdk runs everything on this. Two threads is enough for the transport plus the
  // channel dispatch, and keeping it small makes the ordering easier to reason about.
  // It must never run on Flutter's platform thread.
  boost::asio::io_context io_context;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard;
  std::vector<std::thread> io_threads;
  bool running = false;
};

extern "C" {

void aa_string_free(char* message) { free(message); }

AaSession* aa_session_create(const AaConfig* config, AaEventCallback on_event) {
  auto* session = new AaSession(on_event);
  if (config != nullptr) {
    session->config.width = config->width > 0 ? config->width : 1280;
    session->config.height = config->height > 0 ? config->height : 720;
    session->config.fps = config->fps > 0 ? config->fps : 30;
    session->config.dpi = config->dpi > 0 ? config->dpi : 140;
    session->config.head_unit_name = CopyOrEmpty(config->head_unit_name);
    session->config.car_model = CopyOrEmpty(config->car_model);
    session->config.car_year = CopyOrEmpty(config->car_year);
    session->config.certificate_path = CopyOrEmpty(config->certificate_path);
  }

  session->ring.Configure(session->config.width, session->config.height);
  session->gl = std::make_unique<aa::GlAdapter>(&session->ring);
  session->pattern = std::make_unique<aa::TestPattern>(
      &session->ring, [session]() { session->gl->NotifyFrameAvailable(); });
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
  session->gl.reset();
  if (session->usb) {
    // The connector outlives this session, so cut its link back to it first.
    session->usb->ClearHandlers();
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

  if (!session->usb) {
    session->usb = std::make_unique<aa::UsbConnector>(session->io_context);
  }
  if (!session->channel_strand) {
    session->channel_strand = std::make_unique<aasdk::Strand>(session->io_context);
  }
  const std::string usb_error = session->usb->Start(
      [session](aasdk::usb::IAOAPDevice::Pointer device) {
        // A phone reached accessory mode. Hand it to a fresh protocol session; any
        // previous one belongs to a connection that has already gone away.
        if (session->protocol) {
          session->protocol->Stop();
        }
        session->reached_connected = false;
        session->protocol = aa::ProtocolSession::Create(
            session->io_context, *session->channel_strand, session->Describe(),
            [session](int state, const std::string& message) {
              if (state == AA_STATE_CONNECTED) {
                session->reached_connected = true;
                session->recovery_attempts = 0;
              }

              // A session that fails before it ever connected usually means the phone is
              // still in accessory mode from a run that died without saying goodbye. It
              // will not answer on those endpoints again until it has been through the
              // AOAP handshake, so bounce it and start over rather than reporting a dead
              // end the user can only fix by unplugging the cable.
              if (state == AA_STATE_ERROR && !session->reached_connected &&
                  session->recovery_attempts < kMaxRecoveryAttempts && session->usb) {
                ++session->recovery_attempts;
                session->events.Emit(
                    AA_STATE_SEARCHING,
                    "The phone did not answer, resetting it and trying again (" +
                        std::to_string(session->recovery_attempts) + " of " +
                        std::to_string(kMaxRecoveryAttempts) + ").");
                session->usb->RecoverAndRediscover();
                return;
              }
              session->events.Emit(static_cast<AaState>(state), message);
            });
        session->protocol->Start(std::move(device));
      },
      [session](const std::string& message) {
        session->events.Emit(AA_STATE_ERROR, message);
      });

  if (!usb_error.empty()) {
    session->events.Emit(AA_STATE_ERROR, usb_error);
    return -3;
  }

  session->events.Emit(AA_STATE_SEARCHING, "Looking for a phone on USB.");
  return 0;
}

int32_t aa_session_stop(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if (session->pattern) {
    session->pattern->Stop();
  }
  // Ask, but do not yet destroy. Both of these only queue cancellations onto the
  // io_context, and those queued handlers still need libusb and the USB device to be
  // alive when they run.
  if (session->protocol) {
    // Say goodbye and actually wait to be heard. A fixed sleep was not enough: the
    // phone keeps Android Auto running, and its claim on the USB interface, until it
    // acknowledges. Waiting for the acknowledgement is what makes resuming work.
    session->protocol->Shutdown();
    const bool acknowledged =
        session->protocol->WaitForShutdown(std::chrono::milliseconds(1500));
    if (!acknowledged) {
      session->events.Emit(
          AA_STATE_IDLE,
          "The phone did not acknowledge the shutdown. It may keep Android Auto running, "
          "which can delay the next connection.");
    }
    session->protocol->Stop();
  }
  if (session->usb) {
    session->usb->Stop();
    // Bounce the phone out of accessory mode so a later start redoes the AOAP handshake
    // from scratch. Without this the phone stays in accessory mode with its Android Auto
    // session closed, and the next version request simply times out.
    session->usb->ResetDevice();
  }
  if (!session->running) {
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
  session->protocol.reset();

  session->io_context.restart();
  session->running = false;

  session->events.Emit(AA_STATE_IDLE);
  return 0;
}

int64_t aa_session_texture_id(AaSession* session) {
  if (session == nullptr || !session->gl) {
    return -1;
  }
  return session->gl->texture_id();
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
