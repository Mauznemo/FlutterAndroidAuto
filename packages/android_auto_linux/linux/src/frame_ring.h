// SPDX-License-Identifier: GPL-3.0-or-later
// The seam between "something produced a video frame" and "Flutter shows it".
//
// The whole point of this file is that nothing above it names an OpenGL type. Producers
// (the H.264 decoder, the test pattern) publish an AaFrame; the present adapter under
// src/present/ imports it into whatever graphics API Flutter is using today. See the
// Vulkan readiness section in docs/architecture.md.

#ifndef ANDROID_AUTO_LINUX_FRAME_RING_H_
#define ANDROID_AUTO_LINUX_FRAME_RING_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace aa {

enum class FrameKind {
  // Nothing published yet.
  kNone,
  // Tightly packed 8 bit RGBA in host memory. Used by the test pattern and by the
  // software decode fallback when there is no hardware path.
  kCpuRgba,
  // DRM prime file descriptors. The zero copy path, and the one that survives the move
  // to Vulkan, since a dmabuf imports into both EGL and Vulkan.
  kDmabuf,
};

// One separately importable image inside a dmabuf frame.
//
// DRM calls these layers rather than planes, and the distinction matters here: VA-API
// exports an NV12 surface as two layers, a full size single channel luma image and a
// half size two channel chroma image, usually pointing into the same buffer at
// different offsets. Each one imports on its own, as its own texture, which is exactly
// the shape both EGL and Vulkan want. A "plane" in the libavutil sense would not be.
struct FrameLayer {
  int fd = -1;
  uint32_t offset = 0;
  uint32_t pitch = 0;
  uint64_t modifier = 0;
  // DRM fourcc of this layer on its own, so DRM_FORMAT_R8 or DRM_FORMAT_GR88 for the
  // two halves of an NV12 frame, not DRM_FORMAT_NV12.
  uint32_t fourcc = 0;
  int32_t width = 0;
  int32_t height = 0;
};

// Which matrix takes the frame's YUV back to RGB. H.264 carries this in the VUI, and
// getting it wrong is a subtle colour shift rather than an obvious failure, so it is
// carried through the seam rather than assumed at the far end.
enum class ColorSpace {
  kBt601,
  kBt709,
};

// What a producer hands over. Deliberately free of graphics API types.
struct Frame {
  static constexpr int kMaxLayers = 3;

  FrameKind kind = FrameKind::kNone;
  // The whole decoded image, margins included.
  int32_t width = 0;
  int32_t height = 0;

  // What to cut off each edge before showing it, the black the phone was asked to leave
  // round its interface. See src/video/video_margins.h. The present adapter hands Flutter
  // only what is inside, so a texture never has black bars of its own to letterbox.
  int32_t crop_top = 0;
  int32_t crop_bottom = 0;
  int32_t crop_left = 0;
  int32_t crop_right = 0;

  int32_t visible_width() const { return width - crop_left - crop_right; }
  int32_t visible_height() const { return height - crop_top - crop_bottom; }

  // kCpuRgba
  const uint8_t* pixels = nullptr;
  int32_t stride = 0;

  // kDmabuf
  int32_t layer_count = 0;
  FrameLayer layers[kMaxLayers];
  ColorSpace color_space = ColorSpace::kBt601;
  // Whether luma spans 0..255 rather than the broadcast 16..235.
  bool full_range = false;
};

// A three slot rotation between one producer thread and Flutter's raster thread.
//
// Three is the smallest number that lets the producer keep working while the raster
// thread is still uploading: at most one slot is being read and one is published, so
// there is always a third to write into. The lock is only ever held to move an index,
// never across an upload, which keeps the raster thread off the producer's critical
// path.
//
// Producers come in two shapes and the ring serves both. The test pattern copies its
// pixels into a slot the ring owns (AcquireWriteSlot, SlotPixels, Publish). The decoder
// already has a buffer, on the GPU in the dmabuf case, and only hands over a
// description of it plus something that keeps it alive (PublishFrame).
class FrameRing {
 public:
  static constexpr int kSlots = 3;

  // Resizes the backing buffers used by the copy-in path. Safe to call when the video
  // resolution changes; any frame currently checked out stays valid until it is
  // released.
  void Configure(int32_t width, int32_t height);

  // Copy-in producer side. Returns the slot to write into, or -1 if every slot is busy,
  // which means the consumer is running late and this frame should be dropped.
  int AcquireWriteSlot();
  uint8_t* SlotPixels(int slot);
  void Publish(int slot);

  // Publish-by-reference producer side, for a frame whose memory the producer owns.
  //
  // `keepalive` holds whatever the descriptor points at, an AVFrame in practice. It is
  // released only when the slot is reused, so the consumer can never be handed
  // descriptors whose buffer has already gone. Returns false when every slot is in use
  // and the frame was dropped.
  bool PublishFrame(const Frame& frame, std::shared_ptr<void> keepalive);

  // Consumer side. Returns false when there is no new frame. The returned Frame stays
  // valid until ReleaseRead.
  bool AcquireRead(Frame* out);
  void ReleaseRead();

  // Forgets everything published so far and drops the producer's buffers. Called when a
  // video stream ends, so the last frame of a finished session does not sit in the
  // texture holding a dmabuf open.
  void Reset();

  int32_t width() const;
  int32_t height() const;
  // Diagnostics only.
  uint64_t published_count() const;
  uint64_t dropped_count() const;

 private:
  struct Slot {
    // Only used by the copy-in path. Empty for frames published by reference.
    std::vector<uint8_t> storage;
    Frame frame;
    std::shared_ptr<void> keepalive;
    bool writing = false;
  };

  // Takes the slot the producer should write into, or -1 when they are all busy.
  // Caller holds the lock.
  int TakeFreeSlotLocked();

  mutable std::mutex mutex_;
  Slot slots_[kSlots];
  int published_ = -1;
  int reading_ = -1;
  int32_t width_ = 0;
  int32_t height_ = 0;
  uint64_t published_count_ = 0;
  uint64_t dropped_count_ = 0;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_FRAME_RING_H_
