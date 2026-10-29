// SPDX-License-Identifier: GPL-3.0-or-later
// UsbConnector on Android, the same interface as usb_connector.h over a different way in.
//
// On Linux libusb finds phones itself, through hotplug. On Android it cannot: an app
// sees no USB device until UsbManager hands one over as an open file descriptor, which
// it only does once the user has allowed it. So discovery lives in Java (see
// AndroidAutoPlugin.java), and this takes each descriptor it is given:
//
//   a phone in normal mode   wrapped with libusb_wrap_sys_device and sent the AOAP
//                            handshake, after which it drops off the bus and comes back
//                            as an accessory, which Java hands over in turn
//   a phone in accessory     wrapped and opened as an AOAPDevice, exactly what the Linux
//                            connector hands the session
//
// Everything past the descriptor is aasdk's own, the same query chain and the same
// AOAPDevice the Linux hub uses.

#ifndef ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_ANDROID_H_
#define ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_ANDROID_H_

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <libusb.h>

#include <aasdk/Common/Strand.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>
#include <aasdk/USB/IAccessoryModeQueryChain.hpp>

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

  // The same five as on Linux, see usb_connector.h for what each is for.
  std::string Start(DeviceHandler on_device, ErrorHandler on_error);
  void ResetDevice();
  void RecoverAndRediscover();
  void ClearHandlers();
  void Stop();

 private:
  // On the strand, for a descriptor Java has handed over.
  void HandleDevice(int fd, bool accessory, const std::string& name);
  void OpenAccessory(aasdk::usb::DeviceHandle handle, const std::string& name, int attempt);
  void SwitchToAccessory(aasdk::usb::DeviceHandle handle, const std::string& name);
  // A libusb handle over `fd`, whose last reference closes it and gives the descriptor
  // back to Java. Null if libusb will not take it.
  aasdk::usb::DeviceHandle Wrap(int fd);
  void ReportError(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand strand_;
  UsbContext* usb_context_ = nullptr;
  std::atomic<bool> running_{false};
  // The phone in accessory mode, kept so it can be bounced. Strand only.
  aasdk::usb::DeviceHandle last_handle_;

  std::unique_ptr<aasdk::usb::AccessoryModeQueryFactory> query_factory_;
  std::unique_ptr<aasdk::usb::AccessoryModeQueryChainFactory> query_chain_factory_;
  // Handshakes in flight, kept alive until they finish. Strand only.
  std::list<aasdk::usb::IAccessoryModeQueryChain::Pointer> chains_;
  std::unique_ptr<boost::asio::steady_timer> retry_timer_;

  DeviceHandler on_device_;
  ErrorHandler on_error_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_USB_CONNECTOR_ANDROID_H_
