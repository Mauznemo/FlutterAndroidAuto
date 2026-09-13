// Finds a phone on USB and gets it into Android Auto accessory mode.
//
// The phone starts life as an ordinary USB device. AOAP (Android Open Accessory
// Protocol) is the handshake that asks it to re-enumerate as an accessory, and
// announcing ourselves as manufacturer "Android", model "Android Auto" is what makes it
// offer projection rather than a generic accessory session. aasdk does the actual
// query chain; this wraps it in a lifecycle the rest of the plugin can drive.

#ifndef ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_H_
#define ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_H_

#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <libusb.h>

#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/ConnectedAccessoriesEnumerator.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/USBWrapper.hpp>

#include "usb_context.h"

namespace aa {

class UsbConnector {
 public:
  using DeviceHandler = std::function<void(aasdk::usb::IAOAPDevice::Pointer)>;
  using ErrorHandler = std::function<void(const std::string&)>;

  explicit UsbConnector(boost::asio::io_context& io_context);
  ~UsbConnector();

  UsbConnector(const UsbConnector&) = delete;
  UsbConnector& operator=(const UsbConnector&) = delete;

  // Starts watching for phones. `on_device` fires on the io_context once a device has
  // reached accessory mode. Returns an error string on failure, empty on success.
  //
  // Safe to call again after Stop(): the aasdk objects underneath are created once and
  // reused, never rebuilt. That is deliberate. USBHub registers a libusb hotplug
  // callback holding a raw pointer to itself and calls shared_from_this() when it
  // fires; destroying the hub while that callback is still registered means the next
  // device arrival throws std::bad_weak_ptr from inside libusb's callback, which
  // terminates the process.
  std::string Start(DeviceHandler on_device, ErrorHandler on_error);

  // Drops the handlers installed by Start. The connector outlives the session that
  // installed them, so this has to be called before that session goes away or a later
  // device arrival calls into freed memory.
  void ClearHandlers();

  // Stops looking for phones. This only queues cancellations onto the io_context, so
  // the owner still has to drain it before destroying this object. libusb itself is
  // process wide and is never shut down, see usb_context.h.
  void Stop();

 private:
  // `attempt` counts consecutive failures to claim the USB interface, see the retry
  // note in the .cc file.
  void WaitForDevice(int attempt = 0);
  void EnumerateAlreadyConnected();
  void RetryAfterBusyInterface(int attempt);

  boost::asio::io_context& io_context_;
  UsbContext* usb_context_ = nullptr;

  std::unique_ptr<aasdk::usb::AccessoryModeQueryFactory> query_factory_;
  std::unique_ptr<aasdk::usb::AccessoryModeQueryChainFactory> query_chain_factory_;
  aasdk::usb::IUSBHub::Pointer hub_;
  aasdk::usb::IConnectedAccessoriesEnumerator::Pointer enumerator_;

  std::unique_ptr<boost::asio::steady_timer> retry_timer_;

  DeviceHandler on_device_;
  ErrorHandler on_error_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_H_
