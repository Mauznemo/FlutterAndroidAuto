#include "usb_connector.h"

#include <algorithm>

#include <aasdk/USB/AOAPDevice.hpp>

namespace aa {
namespace {

// Reconnecting can find the USB interface still claimed, either by the session that
// just ended or by the phone's side of the accessory session, which takes its own time
// to wind down. Measured on a Pixel, a clean handover is quick but an ungraceful one
// can take upwards of ten seconds.
//
// Giving up early is the wrong behaviour for a head unit: a car keeps trying until the
// phone is ready or the driver unplugs it. So the budget is generous, with the interval
// backing off so the common case is still fast.
constexpr int kMaxClaimAttempts = 40;
constexpr int kClaimRetryMinMs = 250;
constexpr int kClaimRetryMaxMs = 2000;
// Bouncing the device is a big hammer: it drops the phone out of accessory mode
// entirely, and it then takes several seconds to come back, during which a connection
// attempt lands mid re-enumeration and fails. That made fresh starts flaky when it was
// tried early, so it is now a genuine last resort, used only once the polite retries
// have had a good ten seconds to work.
constexpr int kResetAfterAttempts = 24;

}  // namespace

UsbConnector::UsbConnector(boost::asio::io_context& io_context)
    : io_context_(io_context) {}

UsbConnector::~UsbConnector() { Stop(); }

std::string UsbConnector::Start(DeviceHandler on_device, ErrorHandler on_error) {
  on_device_ = std::move(on_device);
  on_error_ = std::move(on_error);

  std::string error;
  usb_context_ = UsbContext::Shared(&error);
  if (usb_context_ == nullptr) {
    return error;
  }

  if (hub_ == nullptr) {
    auto& wrapper = usb_context_->wrapper();
    query_factory_ =
        std::make_unique<aasdk::usb::AccessoryModeQueryFactory>(wrapper, io_context_);
    query_chain_factory_ = std::make_unique<aasdk::usb::AccessoryModeQueryChainFactory>(
        wrapper, io_context_, *query_factory_);

    retry_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    hub_ =
        std::make_shared<aasdk::usb::USBHub>(wrapper, io_context_, *query_chain_factory_);
    enumerator_ = std::make_shared<aasdk::usb::ConnectedAccessoriesEnumerator>(
        wrapper, io_context_, *query_chain_factory_);
  }

  // Two paths to a device, and both are needed. The hub catches a phone plugged in
  // after we start. The enumerator catches one that was already plugged in, which is
  // the common case when the app launches in a car that is already wired up.
  WaitForDevice();
  EnumerateAlreadyConnected();
  return {};
}

void UsbConnector::WaitForDevice(int attempt) {
  auto promise = aasdk::usb::IUSBHub::Promise::defer(io_context_);
  promise->then(
      [this, attempt](aasdk::usb::DeviceHandle handle) {
        // Keep a copy: create() takes the handle by value, and a failed attempt still
        // needs something to reset.
        aasdk::usb::DeviceHandle retained = handle;
        last_handle_ = retained;
        aasdk::usb::IAOAPDevice::Pointer device;
        try {
          device = aasdk::usb::AOAPDevice::create(usb_context_->wrapper(), io_context_,
                                                  std::move(handle));
        } catch (const aasdk::error::Error& error) {
          // Throwing here would escape into an io_context thread and take the process
          // with it, so this has to be caught whatever the cause.
          if (error.getCode() == aasdk::error::ErrorCode::USB_CLAIM_INTERFACE &&
              attempt < kMaxClaimAttempts) {
            // A phone whose previous accessory session was not shut down cleanly can sit
            // in accessory mode with its interface claimed and never let go, which no
            // amount of waiting fixes. Bouncing the device is the same thing that
            // unplugging and replugging the cable does, and it is what a car head unit
            // effectively does when the driver gets impatient.
            if (attempt == kResetAfterAttempts && retained != nullptr) {
              libusb_reset_device(retained.get());
            }
            RetryAfterBusyInterface(attempt + 1);
            return;
          }
          if (on_error_) {
            on_error_(std::string("Could not open the phone in accessory mode: ") +
                      error.what());
          }
          WaitForDevice();
          return;
        }
        if (device == nullptr) {
          if (on_error_) {
            on_error_("The phone reached accessory mode but had no usable endpoints.");
          }
          // Keep listening: a replug may well work.
          WaitForDevice();
          return;
        }
        if (on_device_) {
          on_device_(std::move(device));
        }
        // Re-arm immediately. USBHub::handleDevice does nothing at all while its promise
        // is null, and handing the device over clears it, so without this the very next
        // arrival is ignored. That is what makes a phone that is unplugged and plugged
        // back in never come back.
        WaitForDevice();
      },
      [this](const aasdk::error::Error& error) {
        if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
          return;
        }
        if (on_error_) {
          on_error_(std::string("USB hub error: ") + error.what());
        }
      });
  hub_->start(std::move(promise));
}

void UsbConnector::ResetDevice() {
  if (last_handle_ == nullptr) {
    return;
  }
  libusb_reset_device(last_handle_.get());
  last_handle_.reset();
}

void UsbConnector::RecoverAndRediscover() {
  ResetDevice();
  // Re-registering the hotplug callback is what makes libusb enumerate the device that
  // is already attached, so cancel first and then ask again.
  if (hub_) {
    hub_->cancel();
  }
  if (retry_timer_) {
    // The phone needs a moment to come back after a reset, and asking too early just
    // burns an attempt on a device that is still re-enumerating.
    retry_timer_->expires_after(std::chrono::milliseconds(1500));
    retry_timer_->async_wait([this](const boost::system::error_code& ec) {
      if (!ec) {
        WaitForDevice();
        EnumerateAlreadyConnected();
      }
    });
  }
}

void UsbConnector::ClearHandlers() {
  on_device_ = nullptr;
  on_error_ = nullptr;
}

void UsbConnector::RetryAfterBusyInterface(int attempt) {
  const int delay =
      std::min(kClaimRetryMaxMs, kClaimRetryMinMs * (1 + attempt / 2));
  retry_timer_->expires_after(std::chrono::milliseconds(delay));
  retry_timer_->async_wait([this, attempt](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    // The hub's hotplug callback only fires for arrivals. Re-registering it is what
    // makes libusb enumerate the device that is already sitting there, so cancel first
    // and then ask again.
    hub_->cancel();
    WaitForDevice(attempt);
  });
}

void UsbConnector::EnumerateAlreadyConnected() {
  auto promise = aasdk::usb::IConnectedAccessoriesEnumerator::Promise::defer(io_context_);
  promise->then(
      [](bool) {
        // Nothing to do. A device that was switched into accessory mode re-enumerates,
        // and the hub's hotplug callback picks it up from there.
      },
      [this](const aasdk::error::Error& error) {
        if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
          return;
        }
        if (on_error_) {
          on_error_(std::string("Could not enumerate connected accessories: ") +
                    error.what());
        }
      });
  enumerator_->enumerate(std::move(promise));
}

void UsbConnector::Stop() {
  if (retry_timer_) {
    retry_timer_->cancel();
  }
  if (enumerator_) {
    enumerator_->cancel();
  }
  if (hub_) {
    hub_->cancel();
  }

  // Nothing is released here. cancel() only queues work onto the io_context, and
  // USBHub's queued lambda calls libusb_hotplug_deregister_callback, so libusb has to
  // still be usable when the io_context drains. Since libusb is process wide and never
  // shut down, that is automatic.
}

}  // namespace aa
