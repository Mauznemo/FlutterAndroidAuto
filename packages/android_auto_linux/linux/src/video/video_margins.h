// SPDX-License-Identifier: GPL-3.0-or-later
// How much of the phone's frame the head unit actually shows.
//
// The protocol names five frame sizes and all of them are 16:9, so a screen of any other
// shape asks for the nearest one and tells the phone to keep margins clear. The phone
// lays its interface out in the rectangle inside them and fills the margins with black,
// and the head unit crops them off again before the frame reaches Flutter. The phone
// still encodes the whole frame; nothing about the stream changes but what is in it.
//
// Measured on a Pixel 8 Pro, and none of it is written down in the schema:
//
//   - The inner rectangle is centred. A margin announced as one number during service
//     discovery is split evenly between two opposite sides.
//   - Touch coordinates are relative to the top left corner of the inner rectangle, not
//     of the frame, and are not scaled. So the head unit keeps announcing a touchscreen
//     the size of the whole frame and sends positions within the visible picture, which
//     is the only arrangement that also survives the margins changing mid session.
//
// Nothing in this file names a protobuf type, so the decoder can use it too.

#ifndef ANDROID_AUTO_LINUX_VIDEO_VIDEO_MARGINS_H_
#define ANDROID_AUTO_LINUX_VIDEO_VIDEO_MARGINS_H_

#include <cstdint>
#include <string>

namespace aa {

// Pixels of the frame the phone leaves black, per side.
struct VideoMargins {
  int32_t top = 0;
  int32_t bottom = 0;
  int32_t left = 0;
  int32_t right = 0;

  // The totals service discovery carries, which only say how much and not where.
  int32_t horizontal() const { return left + right; }
  int32_t vertical() const { return top + bottom; }
  bool empty() const { return horizontal() == 0 && vertical() == 0; }

  bool operator==(const VideoMargins& other) const {
    return top == other.top && bottom == other.bottom && left == other.left &&
           right == other.right;
  }
  bool operator!=(const VideoMargins& other) const { return !(*this == other); }
};

// The margins that give a `frame_width` by `frame_height` frame a visible picture the
// shape of a `view_width` by `view_height` view.
//
// Only the view's aspect ratio matters, so logical pixels are as good as physical ones.
// Zero margins when the view has no size yet. The visible size is rounded to a multiple
// of four, which keeps every side's margin even, so the half size chroma plane of an NV12
// frame is cropped on a whole sample.
VideoMargins MarginsForView(int32_t frame_width, int32_t frame_height, double view_width,
                            double view_height);

// "1280x552 of 1280x720", for logging.
std::string DescribeMargins(int32_t frame_width, int32_t frame_height,
                            const VideoMargins& margins);

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_VIDEO_MARGINS_H_
