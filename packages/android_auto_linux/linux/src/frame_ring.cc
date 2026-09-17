// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_ring.h"

#include <utility>

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
    slots_[i].storage.assign(bytes, 0);
  }
  published_ = -1;
}

int FrameRing::TakeFreeSlotLocked() {
  for (int i = 0; i < kSlots; ++i) {
    if (i == reading_ || i == published_ || slots_[i].writing) {
      continue;
    }
    return i;
  }
  return -1;
}

int FrameRing::AcquireWriteSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  const int slot = TakeFreeSlotLocked();
  if (slot == -1) {
    ++dropped_count_;
    return -1;
  }
  const size_t bytes = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4;
  if (slots_[slot].storage.size() != bytes) {
    slots_[slot].storage.assign(bytes, 0);
  }
  slots_[slot].writing = true;
  return slot;
}

uint8_t* FrameRing::SlotPixels(int slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot < 0 || slot >= kSlots || slots_[slot].storage.empty()) {
    return nullptr;
  }
  return slots_[slot].storage.data();
}

void FrameRing::Publish(int slot) {
  // Released outside the lock: dropping the last reference to a decoded frame can run
  // an arbitrary deleter, and holding a mutex across that invites a lock order problem
  // for no benefit.
  std::shared_ptr<void> recycled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 0 || slot >= kSlots) {
      return;
    }
    recycled = std::move(slots_[slot].keepalive);
    slots_[slot].writing = false;
    slots_[slot].frame = Frame{};
    slots_[slot].frame.kind = FrameKind::kCpuRgba;
    slots_[slot].frame.width = width_;
    slots_[slot].frame.height = height_;
    slots_[slot].frame.pixels = slots_[slot].storage.data();
    slots_[slot].frame.stride = width_ * 4;

    // Whatever was published and never read is simply superseded. Showing the newest
    // frame matters more than showing every frame.
    if (published_ != -1 && published_ != slot) {
      ++dropped_count_;
    }
    published_ = slot;
    ++published_count_;
  }
}

bool FrameRing::PublishFrame(const Frame& frame, std::shared_ptr<void> keepalive) {
  std::shared_ptr<void> recycled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const int slot = TakeFreeSlotLocked();
    if (slot == -1) {
      ++dropped_count_;
      return false;
    }
    // The frame this slot used to hold is only released now, which is the point: its
    // descriptor stayed valid for as long as any consumer could still have been looking
    // at it.
    recycled = std::move(slots_[slot].keepalive);
    slots_[slot].keepalive = std::move(keepalive);
    slots_[slot].frame = frame;
    // Nothing is copied into the slot's own storage, so make sure a stale pointer into
    // it cannot survive from an earlier copy-in frame.
    if (frame.kind != FrameKind::kCpuRgba) {
      slots_[slot].frame.pixels = nullptr;
    }

    if (published_ != -1 && published_ != slot) {
      ++dropped_count_;
    }
    published_ = slot;
    ++published_count_;
    width_ = frame.width;
    height_ = frame.height;
  }
  return true;
}

bool FrameRing::AcquireRead(Frame* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (published_ == -1 || slots_[published_].frame.kind == FrameKind::kNone) {
    return false;
  }
  reading_ = published_;
  published_ = -1;
  *out = slots_[reading_].frame;
  return true;
}

void FrameRing::ReleaseRead() {
  std::lock_guard<std::mutex> lock(mutex_);
  reading_ = -1;
}

void FrameRing::Reset() {
  std::shared_ptr<void> recycled[kSlots];
  {
    std::lock_guard<std::mutex> lock(mutex_);
    published_ = -1;
    for (int i = 0; i < kSlots; ++i) {
      // A slot the consumer is still holding keeps its buffer. Pulling it out from
      // under an upload in progress is the one thing this class exists to prevent.
      if (i == reading_ || slots_[i].writing) {
        continue;
      }
      recycled[i] = std::move(slots_[i].keepalive);
      slots_[i].frame = Frame{};
    }
  }
}

int32_t FrameRing::width() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return width_;
}

int32_t FrameRing::height() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return height_;
}

uint64_t FrameRing::published_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return published_count_;
}

uint64_t FrameRing::dropped_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_count_;
}

}  // namespace aa
