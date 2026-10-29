// SPDX-License-Identifier: GPL-3.0-or-later
// GlAdapter on Android: the same seam as gl_adapter.cc, a different way in front of
// Flutter.
//
// On Linux Flutter calls populate() on its raster thread and the adapter draws inside
// Flutter's own GL context. Android offers nothing like that: a texture there is a
// SurfaceProducer, a Surface somebody else draws into, and Impeller samples whatever
// lands in it whichever of its backends is running. So this adapter has a thread and an
// EGL context of its own, and Flutter's context is never touched, which is also what
// keeps Impeller's Vulkan backend on Android out of the picture entirely.
//
// Each frame takes two passes. The first imports the frame (an AHardwareBuffer straight
// from MediaCodec, or the test pattern's pixels), crops the margins off and draws it
// into a texture of the size the picture is shown at. The second copies that texture
// into the surface. The middle texture is what lets a replaced surface, after a resize
// or a return from the background, be drawn again at once: Android Auto only sends a
// frame when its screen changes, so waiting for the next one could mean a blank texture
// for as long as the driver looks at a still map.

#include "gl_adapter.h"

// eglCreateImageKHR and eglDestroyImageKHR are exported by every Android libEGL.
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// After gl3.h, which defines the types it uses.
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../frame_ring.h"
#include "../platform/android_jni.h"

namespace aa {
namespace {

constexpr const char* kTag = "AndroidAuto";

// What the surface starts out as, before the first frame says how big the picture is.
constexpr int32_t kInitialWidth = 1280;
constexpr int32_t kInitialHeight = 720;

constexpr const char* kVertexShader = R"(
attribute vec2 a_position;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
  v_uv = a_uv;
  gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

constexpr const char* kExternalFragmentShader = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 v_uv;
uniform samplerExternalOES u_texture;
void main() {
  gl_FragColor = texture2D(u_texture, v_uv);
}
)";

constexpr const char* kPlainFragmentShader = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_texture;
void main() {
  gl_FragColor = texture2D(u_texture, v_uv);
}
)";

using GetNativeClientBuffer = EGLClientBuffer (*)(const AHardwareBuffer*);
using ImageTargetTexture = void (*)(GLenum, GLeglImageOES);

void LogGl(const char* message) {
  __android_log_print(ANDROID_LOG_WARN, kTag, "[Present] %s", message);
}

GLuint Compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "[Present] shader: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

struct Program {
  GLuint id = 0;
  GLint position = -1;
  GLint uv = -1;
  GLint texture = -1;
};

Program Link(const char* fragment) {
  Program program;
  GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
  GLuint fs = Compile(GL_FRAGMENT_SHADER, fragment);
  if (vs == 0 || fs == 0) {
    return program;
  }
  program.id = glCreateProgram();
  glAttachShader(program.id, vs);
  glAttachShader(program.id, fs);
  glLinkProgram(program.id);
  glDeleteShader(vs);
  glDeleteShader(fs);
  program.position = glGetAttribLocation(program.id, "a_position");
  program.uv = glGetAttribLocation(program.id, "a_uv");
  program.texture = glGetUniformLocation(program.id, "u_texture");
  return program;
}

// Draws a quad over the whole viewport, sampling `u0..u1` by `v0..v1`. `v0` is the
// first row in memory, which is the top of the picture, and GL's viewport puts y = -1 at
// the bottom, so the top edge of the quad takes v0.
void DrawQuad(const Program& program, float u0, float v0, float u1, float v1) {
  const GLfloat positions[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
  const GLfloat uvs[] = {u0, v1, u1, v1, u0, v0, u1, v0};
  glUseProgram(program.id);
  glUniform1i(program.texture, 0);
  glEnableVertexAttribArray(program.position);
  glEnableVertexAttribArray(program.uv);
  glVertexAttribPointer(program.position, 2, GL_FLOAT, GL_FALSE, 0, positions);
  glVertexAttribPointer(program.uv, 2, GL_FLOAT, GL_FALSE, 0, uvs);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(program.position);
  glDisableVertexAttribArray(program.uv);
}

// Owns the render thread, the EGL context and the surface. One per GlAdapter, behind
// its opaque pointer.
class Presenter : public android::SurfaceListener {
 public:
  Presenter(FrameRing* ring, std::atomic<uint64_t>* display_size,
            std::atomic<bool>* clear_requested)
      : ring_(ring), display_size_(display_size), clear_requested_(clear_requested) {}

  ~Presenter() override { Shutdown(); }

  int64_t Register() {
    if (texture_id_ >= 0) {
      return texture_id_;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = false;
    }
    thread_ = std::thread([this]() { Run(); });
    texture_id_ = android::CreateSurface(this, kInitialWidth, kInitialHeight);
    if (texture_id_ < 0) {
      StopThread();
      return -1;
    }
    surface_width_ = kInitialWidth;
    surface_height_ = kInitialHeight;
    return texture_id_;
  }

  void Shutdown() {
    if (texture_id_ >= 0) {
      android::ReleaseSurface(this);
      texture_id_ = -1;
    }
    StopThread();
  }

  void Notify() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_pending_ = true;
    }
    cv_.notify_all();
  }

  void Wake() { cv_.notify_all(); }

  int64_t texture_id() const { return texture_id_; }

  void OnSurfaceAvailable(ANativeWindow* window) override {
    ANativeWindow* superseded = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      superseded = pending_window_;
      pending_window_ = window;
      window_changed_ = true;
      ++requested_window_;
    }
    if (superseded != nullptr) {
      ANativeWindow_release(superseded);
    }
    cv_.notify_all();
  }

  void OnSurfaceCleanup() override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_window_ != nullptr) {
      ANativeWindow_release(pending_window_);
      pending_window_ = nullptr;
    }
    window_changed_ = true;
    const uint64_t wanted = ++requested_window_;
    cv_.notify_all();
    // Flutter destroys the surface once this returns, so the render thread has to have
    // let go of it first. Bounded, so a wedged GPU cannot hang the main thread.
    ack_cv_.wait_for(lock, std::chrono::seconds(1),
                     [this, wanted]() { return adopted_window_ >= wanted || !running_; });
  }

 private:
  void StopThread() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_window_ != nullptr) {
      ANativeWindow_release(pending_window_);
      pending_window_ = nullptr;
    }
  }

  void Run() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = true;
    }
    if (!InitEgl()) {
      LogGl("EGL could not be initialised, nothing will be drawn");
    }
    for (;;) {
      bool window_changed = false;
      bool frame_pending = false;
      ANativeWindow* window = nullptr;
      uint64_t window_generation = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
          return stop_ || frame_pending_ || window_changed_ || clear_requested_->load();
        });
        if (stop_) {
          break;
        }
        window_changed = window_changed_;
        frame_pending = frame_pending_;
        window_changed_ = false;
        frame_pending_ = false;
        if (window_changed) {
          window = pending_window_;
          pending_window_ = nullptr;
          window_generation = requested_window_;
        }
      }

      if (window_changed) {
        AdoptWindow(window);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          adopted_window_ = window_generation;
        }
        ack_cv_.notify_all();
        // Whatever was last shown, shown again on the new surface.
        PresentLast();
      }
      if (clear_requested_->exchange(false)) {
        has_picture_ = false;
        PresentLast();
      }
      if (frame_pending) {
        Frame frame;
        if (ring_->AcquireRead(&frame)) {
          if (RenderFrame(frame)) {
            PresentLast();
          }
          ring_->ReleaseRead();
        }
      }
    }
    AdoptWindow(nullptr);
    TeardownEgl();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    ack_cv_.notify_all();
  }

  bool InitEgl() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
      return false;
    }
    const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_ES3_BIT_KHR,
                                        EGL_SURFACE_TYPE,
                                        EGL_WINDOW_BIT,
                                        EGL_RED_SIZE,
                                        8,
                                        EGL_GREEN_SIZE,
                                        8,
                                        EGL_BLUE_SIZE,
                                        8,
                                        EGL_ALPHA_SIZE,
                                        8,
                                        EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(display_, config_attributes, &config_, 1, &count) || count < 1) {
      return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attributes);
    if (context_ == EGL_NO_CONTEXT) {
      return false;
    }
    // Surfaceless until Flutter hands over a surface: every Android that has API 26 has
    // EGL_KHR_surfaceless_context.
    if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
      return false;
    }
    get_client_buffer_ = reinterpret_cast<GetNativeClientBuffer>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    image_target_ = reinterpret_cast<ImageTargetTexture>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    external_ = Link(kExternalFragmentShader);
    plain_ = Link(kPlainFragmentShader);
    glGenTextures(1, &source_texture_);
    glGenTextures(1, &external_texture_);
    glGenFramebuffers(1, &framebuffer_);
    egl_ready_ = external_.id != 0 && plain_.id != 0 && get_client_buffer_ != nullptr &&
                 image_target_ != nullptr;
    return egl_ready_;
  }

  void TeardownEgl() {
    if (display_ == EGL_NO_DISPLAY) {
      return;
    }
    if (context_ != EGL_NO_CONTEXT) {
      glDeleteTextures(1, &source_texture_);
      glDeleteTextures(1, &external_texture_);
      glDeleteTextures(1, &last_texture_);
      glDeleteFramebuffers(1, &framebuffer_);
      if (external_.id != 0) glDeleteProgram(external_.id);
      if (plain_.id != 0) glDeleteProgram(plain_.id);
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
    }
    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;
    egl_ready_ = false;
  }

  // Swaps the surface being drawn into for `window`, which may be null.
  void AdoptWindow(ANativeWindow* window) {
    if (surface_ != EGL_NO_SURFACE) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
      eglDestroySurface(display_, surface_);
      surface_ = EGL_NO_SURFACE;
    }
    if (window_ != nullptr) {
      ANativeWindow_release(window_);
      window_ = nullptr;
    }
    if (window == nullptr || !egl_ready_) {
      if (window != nullptr) {
        ANativeWindow_release(window);
      }
      return;
    }
    window_ = window;
    surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
      LogGl("Could not create a window surface on Flutter's texture");
      return;
    }
    eglMakeCurrent(display_, surface_, surface_, context_);
    // A slow consumer drops frames rather than stalling this thread.
    eglSwapInterval(display_, 0);
  }

  // The size to draw a picture of `width` by `height` at: the picture's own, unless it
  // is shown smaller, in which case it is drawn at the size it is shown at so Flutter
  // never has to shrink it.
  void TargetSize(int32_t width, int32_t height, int32_t* out_width, int32_t* out_height) {
    const uint64_t packed = display_size_->load();
    const auto display_width = static_cast<int32_t>(packed >> 32);
    const auto display_height = static_cast<int32_t>(packed & 0xffffffffu);
    if (display_width > 0 && display_height > 0 && display_width < width &&
        display_height < height) {
      *out_width = display_width;
      *out_height = display_height;
    } else {
      *out_width = width;
      *out_height = height;
    }
  }

  // Pass one: the frame, cropped, into last_texture_. Returns whether anything was drawn.
  bool RenderFrame(const Frame& frame) {
    if (!egl_ready_ || frame.width <= 0 || frame.height <= 0 ||
        frame.visible_width() <= 0 || frame.visible_height() <= 0) {
      return false;
    }
    int32_t width = 0;
    int32_t height = 0;
    TargetSize(frame.visible_width(), frame.visible_height(), &width, &height);
    if (width != last_width_ || height != last_height_) {
      if (last_texture_ == 0) {
        glGenTextures(1, &last_texture_);
      }
      glBindTexture(GL_TEXTURE_2D, last_texture_);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   nullptr);
      SetFiltering(GL_TEXTURE_2D);
      last_width_ = width;
      last_height_ = height;
    }
    if (width != surface_width_ || height != surface_height_) {
      // The surface follows the picture, so Flutter samples it one to one. The new
      // surface arrives through OnSurfaceAvailable and is drawn from last_texture_.
      surface_width_ = width;
      surface_height_ = height;
      android::ResizeSurface(this, width, height);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           last_texture_, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);

    const float u0 = static_cast<float>(frame.crop_left) / frame.width;
    const float u1 = static_cast<float>(frame.width - frame.crop_right) / frame.width;
    const float v0 = static_cast<float>(frame.crop_top) / frame.height;
    const float v1 = static_cast<float>(frame.height - frame.crop_bottom) / frame.height;

    bool drawn = false;
    glActiveTexture(GL_TEXTURE0);
    if (frame.kind == FrameKind::kHardwareBuffer && frame.hardware_buffer != nullptr) {
      drawn = DrawHardwareBuffer(frame, u0, v0, u1, v1);
    } else if (frame.kind == FrameKind::kCpuRgba && frame.pixels != nullptr) {
      glBindTexture(GL_TEXTURE_2D, source_texture_);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride / 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, frame.pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      SetFiltering(GL_TEXTURE_2D);
      DrawQuad(plain_, u0, v0, u1, v1);
      drawn = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    has_picture_ = has_picture_ || drawn;
    return drawn;
  }

  bool DrawHardwareBuffer(const Frame& frame, float u0, float v0, float u1, float v1) {
    auto* buffer = static_cast<AHardwareBuffer*>(const_cast<void*>(frame.hardware_buffer));
    EGLClientBuffer client = get_client_buffer_(buffer);
    if (client == nullptr) {
      return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image =
        eglCreateImageKHR(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client,
                          attributes);
    if (image == EGL_NO_IMAGE_KHR) {
      if (!image_error_logged_) {
        image_error_logged_ = true;
        LogGl("Could not import a decoded frame into EGL");
      }
      return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_texture_);
    image_target_(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
    SetFiltering(GL_TEXTURE_EXTERNAL_OES);
    DrawQuad(external_, u0, v0, u1, v1);
    // The draw has to have read the buffer before the image goes, and before the ring
    // hands it back to the decoder.
    glFinish();
    eglDestroyImageKHR(display_, image);
    return true;
  }

  // Pass two: last_texture_, or nothing at all, into the surface.
  void PresentLast() {
    if (surface_ == EGL_NO_SURFACE) {
      return;
    }
    EGLint width = 0;
    EGLint height = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &width);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (has_picture_ && last_texture_ != 0) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, last_texture_);
      // The middle texture was drawn top row first, so it is sampled upside down
      // relative to the source: its first row is v = 0 in GL, which is the bottom.
      DrawQuad(plain_, 0.f, 1.f, 1.f, 0.f);
    }
    eglSwapBuffers(display_, surface_);
  }

  static void SetFiltering(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  FrameRing* ring_;
  std::atomic<uint64_t>* display_size_;
  std::atomic<bool>* clear_requested_;
  std::thread thread_;
  int64_t texture_id_ = -1;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable ack_cv_;
  bool stop_ = false;
  bool running_ = false;
  bool frame_pending_ = false;
  bool window_changed_ = false;
  ANativeWindow* pending_window_ = nullptr;
  uint64_t requested_window_ = 0;
  uint64_t adopted_window_ = 0;

  // Render thread only.
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  ANativeWindow* window_ = nullptr;
  bool egl_ready_ = false;
  bool image_error_logged_ = false;
  GetNativeClientBuffer get_client_buffer_ = nullptr;
  ImageTargetTexture image_target_ = nullptr;
  Program external_;
  Program plain_;
  GLuint source_texture_ = 0;
  GLuint external_texture_ = 0;
  GLuint last_texture_ = 0;
  GLuint framebuffer_ = 0;
  int32_t last_width_ = 0;
  int32_t last_height_ = 0;
  bool has_picture_ = false;
  // What the surface was last asked to be. Written on the platform thread by Register,
  // before the render thread has a frame to draw, and on the render thread after.
  int32_t surface_width_ = 0;
  int32_t surface_height_ = 0;
};

Presenter* AsPresenter(void* texture) { return static_cast<Presenter*>(texture); }

}  // namespace

GlAdapter::GlAdapter(FrameRing* ring) : ring_(ring) {
  texture_ = new Presenter(ring_, &display_size_, &clear_requested_);
}

GlAdapter::~GlAdapter() {
  Shutdown();
  delete AsPresenter(texture_);
  texture_ = nullptr;
}

bool GlAdapter::Register() {
  if (texture_id_ >= 0) {
    return true;
  }
  texture_id_ = AsPresenter(texture_)->Register();
  return texture_id_ >= 0;
}

bool GlAdapter::NotifyFrameAvailable() {
  if (texture_id_ < 0) {
    return false;
  }
  AsPresenter(texture_)->Notify();
  return true;
}

int64_t GlAdapter::texture_id() const { return texture_id_; }

void GlAdapter::SetDisplaySize(int32_t width, int32_t height) {
  const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
                          static_cast<uint32_t>(height);
  display_size_.store(packed);
}

void GlAdapter::Clear() {
  clear_requested_.store(true);
  AsPresenter(texture_)->Wake();
}

void GlAdapter::Shutdown() {
  if (texture_ != nullptr) {
    AsPresenter(texture_)->Shutdown();
  }
  texture_id_ = -1;
}

bool GlAdapter::DmabufSupported() {
  // There is no dmabuf on Android, and the decoder there never asks. Its equivalent,
  // the hardware buffer, imports through EGL_ANDROID_get_native_client_buffer, which
  // every Android with API 26 has.
  return true;
}

}  // namespace aa
