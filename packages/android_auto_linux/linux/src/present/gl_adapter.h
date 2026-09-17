// SPDX-License-Identifier: GPL-3.0-or-later
// The only file in this project that is allowed to name an OpenGL type.
//
// It takes an aa::Frame off the FrameRing and gets it in front of Flutter. Today that
// means an FlTextureGL, because Impeller on Linux runs its OpenGLES backend. When Linux
// moves to Impeller Vulkan, a vk_adapter sits beside this file and nothing above the
// FrameRing changes. See docs/architecture.md.

#ifndef ANDROID_AUTO_LINUX_PRESENT_GL_ADAPTER_H_
#define ANDROID_AUTO_LINUX_PRESENT_GL_ADAPTER_H_

#include <cstdint>

namespace aa {

class FrameRing;

// Owns a Flutter texture fed from `ring`.
//
// Construction does not register anything, so a session that never shows video never
// creates a texture.
class GlAdapter {
 public:
  explicit GlAdapter(FrameRing* ring);
  ~GlAdapter();

  GlAdapter(const GlAdapter&) = delete;
  GlAdapter& operator=(const GlAdapter&) = delete;

  // Creates and registers the Flutter texture. Must run on the platform thread, so
  // call it from an FFI entry point rather than from a producer thread.
  bool Register();

  // Tells Flutter a new frame is waiting in the ring. Safe to call from any thread,
  // which is the whole reason registration is a separate step. Returns false if
  // Register has not run.
  bool NotifyFrameAvailable();

  // The Flutter texture id, or -1 before the first frame.
  int64_t texture_id() const;

  // Unregisters the texture. Must run on the platform thread.
  void Shutdown();

  // Whether a dmabuf can be turned into something Flutter will sample.
  //
  // Only the raster thread can answer this, because the answer depends on the context
  // Flutter made current there, so the answer is optimistic until the first populate
  // has run. The decoder asks before choosing a backend: false sends it to software
  // decode rather than to a stream of frames nothing can display. Safe from any thread.
  static bool DmabufSupported();

 private:
  FrameRing* ring_;
  // Actually an AaVideoTexture*, kept opaque so the header stays free of GObject and
  // Flutter types. Only gl_adapter.cc knows what it is.
  void* texture_ = nullptr;
  int64_t texture_id_ = -1;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_PRESENT_GL_ADAPTER_H_
