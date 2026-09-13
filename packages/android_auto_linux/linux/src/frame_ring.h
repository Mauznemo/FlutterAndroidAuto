// The seam between "something produced a video frame" and "Flutter shows it".
//
// The whole point of this file is that nothing above it names an OpenGL type. Producers
// (the H.264 decoder in M4, the test pattern in M2) publish an AaFrame; the present
// adapter under src/present/ imports it into whatever graphics API Flutter is using
// today. See the Vulkan readiness section in docs/architecture.md.

#ifndef ANDROID_AUTO_LINUX_FRAME_RING_H_
#define ANDROID_AUTO_LINUX_FRAME_RING_H_

#include <cstdint>
#include <mutex>
#include <vector>

namespace aa {

enum class FrameKind {
  // Nothing published yet.
  kNone,
  // Tightly packed 8 bit RGBA in host memory. Used by the M2 test pattern and by the
  // software decode fallback when the driver will not give us a dmabuf.
  kCpuRgba,
  // A DRM prime file descriptor. The zero copy path, and the one that survives the move
  // to Vulkan, since a dmabuf imports into both EGL and Vulkan.
  kDmabuf,
};

// What a producer hands over. Deliberately free of graphics API types.
struct Frame {
  FrameKind kind = FrameKind::kNone;
  int32_t width = 0;
  int32_t height = 0;

  // kCpuRgba
  const uint8_t* pixels = nullptr;
  int32_t stride = 0;

  // kDmabuf. Unused until M4.
  int fd = -1;
  uint64_t modifier = 0;
  uint32_t offset = 0;
  uint32_t fourcc = 0;
};

// A three slot rotation between one producer thread and Flutter's raster thread.
//
// Three is the smallest number that lets the producer keep working while the raster
// thread is still uploading: at most one slot is being read and one is published, so
// there is always a third to write into. The lock is only ever held to move an index,
// never across an upload, which keeps the raster thread off the producer's critical
// path.
class FrameRing {
 public:
  static constexpr int kSlots = 3;

  // Resizes the backing buffers. Safe to call when the video resolution changes; any
  // frame currently checked out stays valid until it is released.
  void Configure(int32_t width, int32_t height);

  // Producer side. Returns the slot to write into, or -1 if every slot is busy, which
  // means the consumer is running late and this frame should be dropped.
  int AcquireWriteSlot();
  uint8_t* SlotPixels(int slot);
  void Publish(int slot);

  // Consumer side. Returns false when there is no new frame. The returned Frame stays
  // valid until ReleaseRead.
  bool AcquireRead(Frame* out);
  void ReleaseRead();

  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  // Frames published since the last call. Diagnostics only.
  uint64_t published_count() const { return published_count_; }
  uint64_t dropped_count() const { return dropped_count_; }

 private:
  mutable std::mutex mutex_;
  std::vector<uint8_t> storage_[kSlots];
  bool writing_[kSlots] = {false, false, false};
  int published_ = -1;
  int reading_ = -1;
  int32_t width_ = 0;
  int32_t height_ = 0;
  uint64_t published_count_ = 0;
  uint64_t dropped_count_ = 0;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_FRAME_RING_H_
