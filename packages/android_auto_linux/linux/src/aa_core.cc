#include "aa_core.h"

#include <boost/asio.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "event_bus.h"
#include "frame_ring.h"
#include "present/gl_adapter.h"
#include "test_pattern.h"

namespace {

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

  Config config;
  aa::EventBus events;
  aa::FrameRing ring;
  std::unique_ptr<aa::GlAdapter> gl;
  std::unique_ptr<aa::TestPattern> pattern;

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
    session->io_threads.emplace_back([session]() { session->io_context.run(); });
  }
  session->running = true;

  // M3 replaces this with real USB discovery. Reporting searching here keeps the Dart
  // state machine honest in the meantime.
  session->events.Emit(AA_STATE_SEARCHING,
                       "Transport not implemented yet (milestone M3). The video path is "
                       "live, so the test pattern works.");
  return 0;
}

int32_t aa_session_stop(AaSession* session) {
  if (session == nullptr) {
    return -1;
  }
  if (session->pattern) {
    session->pattern->Stop();
  }
  if (!session->running) {
    return 0;
  }

  session->work_guard.reset();
  session->io_context.stop();
  for (auto& thread : session->io_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  session->io_threads.clear();
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
