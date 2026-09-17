// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the video path from a generated pattern instead of a phone.
//
// Built first, as a way to prove the whole chain (producer thread, FrameRing, present
// adapter, Flutter Texture, widgets composited on top) with no protocol involved, so
// that plugging in a real H.264 decoder left the decoder as the only new variable.
//
// It is supported API rather than scaffolding: `aa_session_start_test_pattern` and its
// Dart counterparts let a host app lay an overlay out with no phone and no cable, and
// it is still the quickest way to tell a video problem from a presentation one, since
// the real decoder publishes into the same ring.

#ifndef ANDROID_AUTO_LINUX_TEST_PATTERN_H_
#define ANDROID_AUTO_LINUX_TEST_PATTERN_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace aa {

class FrameRing;

class TestPattern {
 public:
  // `on_frame` is called after each publish, on the generator thread.
  TestPattern(FrameRing* ring, std::function<void()> on_frame);
  ~TestPattern();

  void Start(int32_t width, int32_t height, int32_t fps);
  void Stop();
  bool running() const { return running_.load(); }

 private:
  void Run(int32_t width, int32_t height, int32_t fps);
  static void Render(uint8_t* pixels, int32_t width, int32_t height, uint64_t frame);

  FrameRing* ring_;
  std::function<void()> on_frame_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_TEST_PATTERN_H_
