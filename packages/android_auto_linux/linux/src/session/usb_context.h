// SPDX-License-Identifier: GPL-3.0-or-later
// One libusb context for the whole process, deliberately never destroyed.
//
// This is a considered decision, not laziness, and it replaced three separate crash
// fixes that were each chasing the same root cause.
//
// aasdk hands out objects that hold libusb resources: USBEndpoint keeps a
// shared_ptr<libusb_device_handle> whose deleter is libusb_close, AOAPDevice keeps a
// reference to the USBWrapper, USBHub's cancellation deregisters a hotplug callback.
// Those objects are destroyed by reference counts dropping on io_context threads and in
// libusb transfer completions, which means the moment of destruction is not something
// the calling code can pin down. Any one of them running after libusb_exit is a
// segfault in a destructor, which is both hard to attribute and impossible to catch.
//
// Rather than trying to order every one of those destructions ahead of libusb_exit,
// libusb is simply never shut down. A libusb context costs a file descriptor and a
// thread; this application is a head unit, so it owns the USB subsystem for its whole
// life anyway. The event pump is started once for the same reason: transfers that are
// cancelled during teardown still need someone to deliver their completions, and those
// completions are what release the last endpoint references.
//
// If this ever needs to become per session again, the prerequisite is upstream changes
// making every aasdk USB object hold shared ownership of the context.

#ifndef ANDROID_AUTO_LINUX_SESSION_USB_CONTEXT_H_
#define ANDROID_AUTO_LINUX_SESSION_USB_CONTEXT_H_

#include <string>

#include <libusb.h>

#include <aasdk/USB/USBWrapper.hpp>

namespace aa {

class UsbContext {
 public:
  // Returns the process wide context, initialising libusb and starting its event pump
  // on first use. Returns nullptr and fills `error` if libusb will not start; a later
  // call will try again.
  static UsbContext* Shared(std::string* error);

  libusb_context* raw() const { return context_; }
  aasdk::usb::USBWrapper& wrapper() const { return *wrapper_; }

 private:
  UsbContext(libusb_context* context, aasdk::usb::USBWrapper* wrapper);
  // No destructor on purpose. See the note above.

  libusb_context* context_;
  aasdk::usb::USBWrapper* wrapper_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_USB_CONTEXT_H_
