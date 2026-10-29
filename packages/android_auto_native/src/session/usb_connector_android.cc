// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_connector_android.h"

#include <chrono>

#include <aasdk/Common/Log.hpp>
#include <aasdk/Error/Error.hpp>
#include <aasdk/USB/AOAPDevice.hpp>

#include "../platform/android_jni.h"

namespace aa {
namespace {

// The same reasoning as the Linux connector's retry: a phone that has just come back as
// an accessory can still have its interface claimed by the session before it, for a
// while. Shorter here, because a bounce on Android costs a permission dialog.
constexpr int kMaxClaimAttempts = 20;
constexpr int kClaimRetryMs = 500;

}  // namespace

bool UsbConnector::PhonePresent() { return android::UsbPhonePresent(); }

UsbConnector::UsbConnector(boost::asio::io_context& io_context)
    : io_context_(io_context), strand_(io_context) {}

UsbConnector::~UsbConnector() { Stop(); }

std::string UsbConnector::Start(DeviceHandler on_device, ErrorHandler on_error) {
  on_device_ = std::move(on_device);
  on_error_ = std::move(on_error);

  std::string error;
  usb_context_ = UsbContext::Shared(&error);
  if (usb_context_ == nullptr) {
    return error;
  }
  if (!query_chain_factory_) {
    auto& wrapper = usb_context_->wrapper();
    query_factory_ =
        std::make_unique<aasdk::usb::AccessoryModeQueryFactory>(wrapper, io_context_);
    query_chain_factory_ = std::make_unique<aasdk::usb::AccessoryModeQueryChainFactory>(
        wrapper, io_context_, *query_factory_);
    retry_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
  }

  running_ = true;
  // Called on the main thread, so it only queues. The connector is process lifetime,
  // like its Linux twin, so `this` outlives every descriptor that can arrive.
  android::StartUsbDiscovery([this](int fd, bool accessory, std::string name) {
    strand_.dispatch([this, fd, accessory, name]() { HandleDevice(fd, accessory, name); });
  });
  return {};
}

aasdk::usb::DeviceHandle UsbConnector::Wrap(int fd) {
  libusb_device_handle* raw = nullptr;
  const int result =
      libusb_wrap_sys_device(usb_context_->raw(), static_cast<intptr_t>(fd), &raw);
  if (result != 0 || raw == nullptr) {
    ReportError(std::string("libusb would not take the device: ") +
                libusb_strerror(static_cast<libusb_error>(result)));
    android::ReleaseUsbDevice(fd);
    return nullptr;
  }
  // libusb_close does not close a wrapped descriptor; Java does, once told.
  return aasdk::usb::DeviceHandle(raw, [fd](libusb_device_handle* handle) {
    libusb_close(handle);
    android::ReleaseUsbDevice(fd);
  });
}

void UsbConnector::HandleDevice(int fd, bool accessory, const std::string& name) {
  if (!running_) {
    android::ReleaseUsbDevice(fd);
    return;
  }
  aasdk::usb::DeviceHandle handle = Wrap(fd);
  if (handle == nullptr) {
    return;
  }
  if (accessory) {
    AASDK_LOG(info) << "[USB] " << name << " is in accessory mode, opening it";
    OpenAccessory(std::move(handle), name, 0);
  } else {
    AASDK_LOG(info) << "[USB] asking " << name << " to switch to accessory mode";
    SwitchToAccessory(std::move(handle), name);
  }
}

void UsbConnector::SwitchToAccessory(aasdk::usb::DeviceHandle handle,
                                     const std::string& name) {
  auto chain = query_chain_factory_->create();
  chains_.push_back(chain);
  auto promise = aasdk::usb::IAccessoryModeQueryChain::Promise::defer(strand_);
  // Either way the chain is finished with and the handle goes, which hands the
  // descriptor back. On success the phone leaves the bus by itself and returns as an
  // accessory, which arrives as a new device.
  promise->then(
      [this, chain, name](aasdk::usb::DeviceHandle) {
        chains_.remove(chain);
        AASDK_LOG(info) << "[USB] " << name
                        << " accepted the switch, waiting for it to come back";
      },
      [this, chain, name](const aasdk::error::Error& error) {
        chains_.remove(chain);
        if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
          return;
        }
        // Not necessarily a fault: anything that answers to the filter in Java but is
        // not a phone with Android Auto ends here.
        AASDK_LOG(info) << "[USB] " << name
                        << " did not switch to accessory mode: " << error.what();
      });
  chain->start(std::move(handle), std::move(promise));
}

void UsbConnector::OpenAccessory(aasdk::usb::DeviceHandle handle, const std::string& name,
                                 int attempt) {
  if (!running_) {
    return;
  }
  aasdk::usb::IAOAPDevice::Pointer device;
  try {
    device = aasdk::usb::AOAPDevice::create(usb_context_->wrapper(), io_context_, handle);
  } catch (const aasdk::error::Error& error) {
    if (error.getCode() == aasdk::error::ErrorCode::USB_CLAIM_INTERFACE &&
        attempt < kMaxClaimAttempts) {
      retry_timer_->expires_after(std::chrono::milliseconds(kClaimRetryMs));
      retry_timer_->async_wait(
          [this, handle, name, attempt](const boost::system::error_code& ec) {
            if (ec) {
              return;
            }
            strand_.dispatch(
                [this, handle, name, attempt]() { OpenAccessory(handle, name, attempt + 1); });
          });
      return;
    }
    ReportError(std::string("Could not open the phone in accessory mode: ") + error.what());
    return;
  }
  if (device == nullptr) {
    ReportError("The phone reached accessory mode but had no usable endpoints.");
    return;
  }
  last_handle_ = handle;
  if (on_device_) {
    on_device_(std::move(device));
  }
}

void UsbConnector::ResetDevice() {
  // Called from the platform thread by aa_session_stop, while nothing is running on
  // the strand any more.
  if (last_handle_ == nullptr) {
    return;
  }
  libusb_reset_device(last_handle_.get());
  last_handle_.reset();
}

void UsbConnector::RecoverAndRediscover() {
  strand_.dispatch([this]() {
    ResetDevice();
    if (!retry_timer_) {
      return;
    }
    // A reset makes the phone leave and come back, which Java announces as an arrival.
    // A reset that did nothing leaves it where it was, and only a rescan offers it
    // again, once its old descriptor has been given back.
    retry_timer_->expires_after(std::chrono::milliseconds(1500));
    retry_timer_->async_wait([this](const boost::system::error_code& ec) {
      if (!ec && running_) {
        android::RescanUsb();
      }
    });
  });
}

void UsbConnector::ClearHandlers() {
  on_device_ = nullptr;
  on_error_ = nullptr;
}

void UsbConnector::Stop() {
  running_ = false;
  android::StopUsbDiscovery();
  if (retry_timer_) {
    retry_timer_->cancel();
  }
  strand_.dispatch([this]() {
    // A copy: a cancelled chain rejects at once, and its handler takes it off the list.
    const auto chains = chains_;
    for (const auto& chain : chains) {
      chain->cancel();
    }
  });
}

void UsbConnector::ReportError(const std::string& message) {
  if (on_error_) {
    on_error_(message);
  }
}

}  // namespace aa
