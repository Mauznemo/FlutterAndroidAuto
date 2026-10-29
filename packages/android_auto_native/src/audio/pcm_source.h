// SPDX-License-Identifier: GPL-3.0-or-later
// Where the microphone's PCM comes from, before it is wrapped in the protocol.
//
// The capture seam, and the mirror of pcm_sink.h: nothing above this line names
// PulseAudio, and nothing below it knows what a channel is. One backend exists today,
// libpulse-simple, for the same reason the playback side starts there. ALSA or a direct
// PipeWire backend would be another file and no change anywhere else.
//
// PcmFormat and PcmDevice come from pcm_sink.h rather than being declared again. Both
// describe a stream and a device, not a direction, and a second copy of them would be a
// second thing to keep in step.
//
// Every method blocks and none of them is thread safe. A source belongs to exactly one
// thread, which is never an io_context thread: Read() waits for the microphone to
// produce the samples, which is what paces the head unit to real time, and waiting on an
// io thread stalls the transport. AudioInput owns that thread.

#ifndef ANDROID_AUTO_LINUX_AUDIO_PCM_SOURCE_H_
#define ANDROID_AUTO_LINUX_AUDIO_PCM_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pcm_sink.h"

namespace aa {

class PcmSource {
 public:
  virtual ~PcmSource() = default;

  // Connects to the audio server and starts capturing. `device` is a PcmDevice::name
  // from ListPcmCaptureDevices, or empty for whatever the server thinks is the default.
  // `label` is what shows up in a mixer's recording tab, which is the only place a
  // person can see that something is listening, so it should say who is listening.
  //
  // Returns false and fills `error` rather than throwing: a head unit with no
  // microphone should still project.
  virtual bool Open(const PcmFormat& format, const std::string& device,
                    const std::string& label, std::string* error) = 0;

  // Fills exactly `size` bytes with interleaved PCM. Blocks until the microphone has
  // produced them, which is what paces the caller to real time. False means the stream
  // broke and the caller should close and reopen it.
  virtual bool Read(uint8_t* data, size_t size) = 0;

  virtual void Close() = 0;

  // Which backend this is, for logs and for the Dart side to report.
  virtual std::string backend_name() const = 0;
};

// Builds the backend this machine has. Never returns null.
std::unique_ptr<PcmSource> CreatePcmSource();

// A source that produces silence at real time, for a machine with no microphone.
// AudioInput falls back to it when an open fails, so the phone gets a stream it can stop
// rather than a channel that opened and then said nothing. The Assistant hears nothing
// and gives up, which is the honest outcome.
std::unique_ptr<PcmSource> CreateSilentPcmSource();

// What the audio server is currently offering as inputs. Empty if it cannot be reached
// at all. Blocks for up to a second, so call it from the platform thread, not an io
// thread.
std::vector<PcmDevice> ListPcmCaptureDevices();

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_AUDIO_PCM_SOURCE_H_
