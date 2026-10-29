// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_pattern.h"

#include <chrono>
#include <cmath>

#include "frame_ring.h"

namespace aa {

TestPattern::TestPattern(FrameRing* ring, std::function<void()> on_frame)
    : ring_(ring), on_frame_(std::move(on_frame)) {}

TestPattern::~TestPattern() { Stop(); }

void TestPattern::Start(int32_t width, int32_t height, int32_t fps) {
  if (running_.exchange(true)) {
    return;
  }
  ring_->Configure(width, height);
  thread_ = std::thread(&TestPattern::Run, this, width, height, fps <= 0 ? 30 : fps);
}

void TestPattern::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void TestPattern::Run(int32_t width, int32_t height, int32_t fps) {
  const auto interval = std::chrono::nanoseconds(1000000000LL / fps);
  auto next = std::chrono::steady_clock::now();
  uint64_t frame = 0;

  while (running_.load()) {
    const int slot = ring_->AcquireWriteSlot();
    if (slot >= 0) {
      uint8_t* pixels = ring_->SlotPixels(slot);
      if (pixels != nullptr) {
        Render(pixels, width, height, frame);
      }
      ring_->Publish(slot);
      if (on_frame_) {
        on_frame_();
      }
    }
    ++frame;

    next += interval;
    std::this_thread::sleep_until(next);
  }
}

// A moving pattern rather than a static one, deliberately: a still image cannot tell
// you whether frames are actually flowing or whether one frame got stuck on screen.
// The sweeping bar makes a stall obvious at a glance in a screenshot.
void TestPattern::Render(uint8_t* pixels, int32_t width, int32_t height,
                         uint64_t frame) {
  const double t = static_cast<double>(frame) / 60.0;
  const int32_t bar = static_cast<int32_t>(
      (0.5 + 0.5 * std::sin(t * 1.2)) * static_cast<double>(width - 1));

  for (int32_t y = 0; y < height; ++y) {
    // Vertical gradient, so a vertically flipped texture is immediately visible.
    const uint8_t base = static_cast<uint8_t>(20 + 60 * y / (height > 1 ? height - 1 : 1));
    uint8_t* row = pixels + static_cast<size_t>(y) * width * 4;

    for (int32_t x = 0; x < width; ++x) {
      // Checkerboard, so scaling and aspect ratio problems show up as distortion.
      const bool light = ((x / 64) + (y / 64)) % 2 == 0;
      uint8_t r = light ? base + 24 : base;
      uint8_t g = light ? base + 16 : base;
      uint8_t b = light ? base + 48 : base + 20;

      // The sweeping bar.
      const int32_t distance = x - bar;
      if (distance > -6 && distance < 6) {
        r = 255;
        g = 140;
        b = 40;
      }

      // Corner markers, so cropping is visible.
      const bool corner = (x < 24 || x >= width - 24) && (y < 24 || y >= height - 24);
      if (corner) {
        r = 40;
        g = 220;
        b = 120;
      }

      uint8_t* px = row + static_cast<size_t>(x) * 4;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }

  // A progress dot that advances one step per second, as a second, coarser liveness
  // signal that survives a screenshot taken at an unlucky moment.
  const int32_t dots = static_cast<int32_t>(frame / 30) % 10;
  for (int32_t d = 0; d <= dots && d < 10; ++d) {
    const int32_t cx = 40 + d * 28;
    for (int32_t y = height / 2 - 8; y < height / 2 + 8 && y < height; ++y) {
      for (int32_t x = cx - 8; x < cx + 8 && x < width; ++x) {
        if (x < 0 || y < 0) {
          continue;
        }
        uint8_t* px = pixels + (static_cast<size_t>(y) * width + x) * 4;
        px[0] = 255;
        px[1] = 255;
        px[2] = 255;
        px[3] = 255;
      }
    }
  }
}

}  // namespace aa
