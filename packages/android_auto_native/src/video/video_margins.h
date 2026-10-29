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

// The margins that give a `frame_width` by `frame_height` frame a visible picture for a
// `view_width` by `view_height` view, in physical pixels.
//
// When the view fits inside the frame, the picture is the view's own size, so it is
// drawn one to one: the phone lays out a screen exactly that big, at its usual density,
// and nothing is resampled. Shrinking a larger picture instead would draw every glyph the
// phone rendered at a fraction of its size, which is what made a stream started in a big
// window and then moved to a small one look so grainy. When the view is larger than the
// frame, the picture is the largest rectangle of the view's shape that fits, and is
// stretched to fill it.
//
// Zero margins when the view has no size yet. The visible size is even, so the totals
// service discovery carries split into two whole sides; a side may be odd, which the
// converter handles by sampling chroma where it really is.
VideoMargins MarginsForView(int32_t frame_width, int32_t frame_height, double view_width,
                            double view_height);

// A frame size the protocol has a name for.
struct FrameSize {
  int32_t width = 0;
  int32_t height = 0;
};

// The frame to ask the phone for when the host app left the choice to the head unit:
// the smallest of 800x480, 1280x720 and 1920x1080 that holds a `view_width` by
// `view_height` view, in physical pixels. So the picture is drawn one to one and never
// stretched, which nothing can do well: detail the phone never encoded cannot be put
// back. 1920x1080 for a view larger than that, and 1280x720 while the view has no size.
//
// `match_view` says whether the picture will be given the view's shape with margins, as
// MarginsForView does, or shown whole and letterboxed. The larger sizes the protocol
// names are left out: nothing here has tried them, and phones are reported to want H.265
// for them, which the decoder does not ask for.
FrameSize FrameSizeForView(double view_width, double view_height, bool match_view);

// "1280x552 of 1280x720", for logging.
std::string DescribeMargins(int32_t frame_width, int32_t frame_height,
                            const VideoMargins& margins);

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_VIDEO_MARGINS_H_
