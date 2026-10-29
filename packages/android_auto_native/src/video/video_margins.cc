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
// connection. A view smaller than this still works: the picture is shrunk to fit it.
constexpr double kMinimumVisibleFraction = 1.0 / 3.0;

// Down to an even number, never up: a picture one pixel smaller than the view is drawn
// one to one beside a one pixel bar, where one a pixel larger would be resampled.
int32_t FloorToEven(double value) {
  return static_cast<int32_t>(std::floor(value / 2.0)) * 2;
}

}  // namespace

VideoMargins MarginsForView(int32_t frame_width, int32_t frame_height, double view_width,
                            double view_height) {
  VideoMargins margins;
  if (frame_width <= 0 || frame_height <= 0 || !(view_width > 0.0) ||
      !(view_height > 0.0) || !std::isfinite(view_width) || !std::isfinite(view_height)) {
    return margins;
  }

  // One to one when the view fits, otherwise scaled down to fit, keeping its shape.
  double scale = std::min({1.0, frame_width / view_width, frame_height / view_height});
  // And back up, shape still kept, for a view too small to be a screen the phone should
  // lay out for. That picture is shrunk on screen instead.
  scale = std::max({scale, frame_width * kMinimumVisibleFraction / view_width,
                    frame_height * kMinimumVisibleFraction / view_height});
  const int32_t visible_width =
      std::clamp(FloorToEven(view_width * scale), 2, frame_width);
  const int32_t visible_height =
      std::clamp(FloorToEven(view_height * scale), 2, frame_height);

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
  // Room for the pixel MarginsForView rounds away, and no more. Any real stretch is
  // visible as blur on text, and a larger frame costs the phone little: what it adds is
  // margin, which is black and encodes to almost nothing.
  constexpr double kSlack = 1.5;
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
    // Fitted inside the view the way AndroidAutoView fits it, whole and centred: the
    // frame holds the view when the fitted picture is not stretched.
    const double scale =
        std::min(view_width / visible_width, view_height / visible_height);
    if (scale <= 1.0 + kSlack / std::min(visible_width, visible_height)) {
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
