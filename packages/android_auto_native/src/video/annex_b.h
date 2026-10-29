// SPDX-License-Identifier: GPL-3.0-or-later
// The little of H.264's byte stream format both decoders need to read for themselves.

#ifndef ANDROID_AUTO_LINUX_VIDEO_ANNEX_B_H_
#define ANDROID_AUTO_LINUX_VIDEO_ANNEX_B_H_

#include <cstddef>
#include <cstdint>

namespace aa {

// Reads the NAL unit types out of an Annex-B buffer and says whether it contains an
// IDR picture. Used only to decide where to resume after the queue overflowed, so a
// cheap scan for start codes is enough; it does not have to be a real parser.
inline bool ContainsKeyframe(const uint8_t* data, size_t size) {
  size_t zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t byte = data[i];
    if (byte == 0x00) {
      ++zeros;
      continue;
    }
    if (byte == 0x01 && zeros >= 2 && i + 1 < size) {
      const uint8_t nal_type = data[i + 1] & 0x1f;
      // 5 is a coded slice of an IDR picture, 7 and 8 are the SPS and PPS that always
      // precede one in this stream.
      if (nal_type == 5 || nal_type == 7) {
        return true;
      }
    }
    zeros = 0;
  }
  return false;
}

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_ANNEX_B_H_
