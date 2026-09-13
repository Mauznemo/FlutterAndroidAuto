#include "usb_context.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace aa {
namespace {

std::mutex g_mutex;
UsbContext* g_instance = nullptr;

}  // namespace

UsbContext::UsbContext(libusb_context* context, aasdk::usb::USBWrapper* wrapper)
    : context_(context), wrapper_(wrapper) {}

UsbContext* UsbContext::Shared(std::string* error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_instance != nullptr) {
    return g_instance;
  }

  libusb_context* context = nullptr;
  const int result = libusb_init(&context);
  if (result != 0) {
    if (error != nullptr) {
      *error = std::string("libusb_init failed: ") +
               libusb_strerror(static_cast<libusb_error>(result));
    }
    return nullptr;
  }

  // Intentionally leaked, along with the context itself. See usb_context.h.
  auto* wrapper = new aasdk::usb::USBWrapper(context);
  g_instance = new UsbContext(context, wrapper);

  // One pump for the life of the process. Detached rather than joined: there is nothing
  // to join it to, and stopping it is exactly what caused cancelled transfers to leave
  // their endpoints undestroyed.
  std::thread([wrapper]() {
    for (;;) {
      wrapper->handleEvents();
    }
  }).detach();

  return g_instance;
}

}  // namespace aa
