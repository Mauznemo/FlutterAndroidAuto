// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_margins.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace aa {
namespace {

// No side of the visible picture is ever allowed below this fraction of the frame.
//
// Not a limit anything has been seen to enforce. It is here for the transient layouts a
// window goes through while it opens, a few pixels tall for one frame, which would
// otherwise be what the phone is told the car's screen looks like for the whole
// connection. A view narrower than this still works: the view letterboxes the rest.
constexpr double kMinimumVisibleFraction = 1.0 / 3.0;

int32_t RoundToFour(double value) {
  return static_cast<int32_t>(std::lround(value / 4.0)) * 4;
}

}  // namespace

VideoMargins MarginsForView(int32_t frame_width, int32_t frame_height, double view_width,
                            double view_height) {
  VideoMargins margins;
  if (frame_width <= 0 || frame_height <= 0 || !(view_width > 0.0) ||
      !(view_height > 0.0) || !std::isfinite(view_width) || !std::isfinite(view_height)) {
    return margins;
  }

  const double view_aspect = view_width / view_height;
  const double frame_aspect = static_cast<double>(frame_width) / frame_height;
  int32_t visible_width = frame_width;
  int32_t visible_height = frame_height;
  if (view_aspect > frame_aspect) {
    // Wider than the frame, so the phone keeps the top and bottom clear.
    const int32_t floor = RoundToFour(frame_height * kMinimumVisibleFraction);
    visible_height =
        std::clamp(RoundToFour(frame_width / view_aspect), floor, frame_height);
  } else {
    const int32_t floor = RoundToFour(frame_width * kMinimumVisibleFraction);
    visible_width = std::clamp(RoundToFour(frame_height * view_aspect), floor, frame_width);
  }

  const int32_t horizontal = frame_width - visible_width;
  const int32_t vertical = frame_height - visible_height;
  margins.left = horizontal / 2;
  margins.right = horizontal - margins.left;
  margins.top = vertical / 2;
  margins.bottom = vertical - margins.top;
  return margins;
}

FrameSize FrameSizeForView(double view_width, double view_height, bool match_view) {
  static constexpr FrameSize kCandidates[] = {{800, 480}, {1280, 720}, {1920, 1080}};
  // How much stretching is let through before a larger frame is worth its cost. Five
  // percent is not visible; the next size up is more than twice the pixels to encode,
  // carry and decode.
  constexpr double kTolerance = 1.05;
  if (!(view_width > 0.0) || !(view_height > 0.0) || !std::isfinite(view_width) ||
      !std::isfinite(view_height)) {
    return {1280, 720};
  }
  for (const FrameSize& size : kCandidates) {
    const VideoMargins margins =
        match_view ? MarginsForView(size.width, size.height, view_width, view_height)
                   : VideoMargins{};
    const double visible_width = size.width - margins.horizontal();
    const double visible_height = size.height - margins.vertical();
    // Fitted inside the view the way AndroidAutoView fits it, whole and centred.
    const double scale =
        std::min(view_width / visible_width, view_height / visible_height);
    if (scale <= kTolerance) {
      return size;
    }
  }
  return kCandidates[std::size(kCandidates) - 1];
}

std::string DescribeMargins(int32_t frame_width, int32_t frame_height,
                            const VideoMargins& margins) {
  return std::to_string(frame_width - margins.horizontal()) + "x" +
         std::to_string(frame_height - margins.vertical()) + " of " +
         std::to_string(frame_width) + "x" + std::to_string(frame_height);
}

}  // namespace aa
