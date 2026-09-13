#include "frame_ring.h"

namespace aa {

void FrameRing::Configure(int32_t width, int32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (width == width_ && height == height_) {
    return;
  }
  width_ = width;
  height_ = height;
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  for (int i = 0; i < kSlots; ++i) {
    // Leave a slot that is currently checked out alone, it is being uploaded right now.
    // It keeps its old dimensions until it comes back, which is correct: the frame in
    // it really is the old size.
    if (i == reading_) {
      continue;
    }
    storage_[i].assign(bytes, 0);
  }
  published_ = -1;
}

int FrameRing::AcquireWriteSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < kSlots; ++i) {
    if (i == reading_ || i == published_ || writing_[i]) {
      continue;
    }
    const size_t bytes =
        static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4;
    if (storage_[i].size() != bytes) {
      storage_[i].assign(bytes, 0);
    }
    writing_[i] = true;
    return i;
  }
  ++dropped_count_;
  return -1;
}

uint8_t* FrameRing::SlotPixels(int slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot < 0 || slot >= kSlots || storage_[slot].empty()) {
    return nullptr;
  }
  return storage_[slot].data();
}

void FrameRing::Publish(int slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot < 0 || slot >= kSlots) {
    return;
  }
  writing_[slot] = false;
  // Whatever was published and never read is simply superseded. Showing the newest
  // frame matters more than showing every frame.
  if (published_ != -1 && published_ != slot) {
    ++dropped_count_;
  }
  published_ = slot;
  ++published_count_;
}

bool FrameRing::AcquireRead(Frame* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (published_ == -1 || storage_[published_].empty()) {
    return false;
  }
  reading_ = published_;
  published_ = -1;

  out->kind = FrameKind::kCpuRgba;
  out->width = width_;
  out->height = height_;
  out->pixels = storage_[reading_].data();
  out->stride = width_ * 4;
  return true;
}

void FrameRing::ReleaseRead() {
  std::lock_guard<std::mutex> lock(mutex_);
  reading_ = -1;
}

}  // namespace aa
