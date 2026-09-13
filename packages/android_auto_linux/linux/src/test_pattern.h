// Milestone M2 only: drives the video path without a phone.
//
// It exists to prove the whole chain (producer thread, FrameRing, present adapter,
// Flutter Texture, widgets composited on top) before any Android Auto protocol work
// starts, so that when M4 plugs a real H.264 decoder into the same FrameRing the only
// new variable is the decoder. Delete it once M4 is done.

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
