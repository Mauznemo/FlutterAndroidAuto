// SPDX-License-Identifier: GPL-3.0-or-later
// H.264 to frames, whichever platform this is.
//
// Each platform has its own aa::VideoDecoder with the same public interface, because
// the two have nothing in common below it: libavcodec with VA-API or the CPU on Linux,
// MediaCodec on Android. The channels and the session only ever include this file.
//
//   libav_decoder.h       Linux
//   mediacodec_decoder.h  Android

#ifndef ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_
#define ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_

#include <cstdint>

#if defined(__ANDROID__)
#include "mediacodec_decoder.h"
#else
#include "libav_decoder.h"
#endif

namespace aa {

// Steady clock reading in microseconds, the one the latency figures are built on.
int64_t NowMicros();

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_
