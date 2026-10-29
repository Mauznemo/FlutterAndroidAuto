// SPDX-License-Identifier: GPL-3.0-or-later
// The only file besides android_jni.cc that names a JNI type.
//
// On Android the core is loaded twice over: Java loads it to register the plugin, and
// Dart opens the same library for FFI. Everything Dart does goes through the C ABI in
// aa_core.h exactly as on Linux. JNI is only for what Java alone can reach, which today
// is Flutter's texture registry: a SurfaceProducer is a Java object, and the Surface it
// hands out is what the present adapter draws into. And USB: an app only reaches a USB
// device through UsbManager, which hands it over as a file descriptor once the user has
// allowed it.

#ifndef ANDROID_AUTO_PLATFORM_ANDROID_JNI_H_
#define ANDROID_AUTO_PLATFORM_ANDROID_JNI_H_

#if defined(__ANDROID__)

#include <cstdint>
#include <functional>
#include <string>

struct ANativeWindow;

namespace aa {
namespace android {

// What a surface's owner is told. Both are called on the main thread.
class SurfaceListener {
 public:
  virtual ~SurfaceListener() = default;

  // A surface to draw into, the first one or a replacement after a resize or a return
  // from the background. Ownership of the reference passes to the listener, which must
  // ANativeWindow_release it when done.
  virtual void OnSurfaceAvailable(ANativeWindow* window) = 0;

  // Flutter is taking the surface away. Must not return until nothing draws into it.
  virtual void OnSurfaceCleanup() = 0;
};

// Creates a Flutter texture backed by a SurfaceProducer `width` by `height` pixels and
// returns its id, or -1 when no engine is attached. `listener` hears about the surface,
// once before this returns and again whenever it changes, until ReleaseSurface. Call on
// the platform thread.
int64_t CreateSurface(SurfaceListener* listener, int32_t width, int32_t height);

// Asks for the texture to be `width` by `height`. Returns at once; the new surface
// arrives through OnSurfaceAvailable. Safe from any thread.
void ResizeSurface(SurfaceListener* listener, int32_t width, int32_t height);

// Releases the texture. The listener hears OnSurfaceCleanup first, and nothing at all
// once this has returned. Call on the platform thread.
void ReleaseSurface(SurfaceListener* listener);

// A USB device the app may now use: an open usbfs file descriptor, whether it is a phone
// already in accessory mode, and a name for the log. Called on the main thread.
//
// The descriptor stays open until ReleaseUsbDevice is called with it. Java owns it and
// must not close it under libusb, which polls it from its event thread.
using UsbDeviceHandler = std::function<void(int fd, bool accessory, std::string name)>;

// Starts watching for phones on USB: everything already attached, then every arrival.
// A device the app has no permission for yet is asked about first, which shows the user
// a dialog. Replaces any previous handler.
void StartUsbDiscovery(UsbDeviceHandler handler);

// Stops watching and drops the handler. Devices already handed over stay open until
// they are released.
void StopUsbDiscovery();

// Offers every attached device again that is not currently handed over. For a phone that
// was left enumerated after its session failed, which no arrival will ever announce.
void RescanUsb();

// Closes a descriptor StartUsbDiscovery handed over. Safe from any thread.
void ReleaseUsbDevice(int fd);

// A phone that opened the RFCOMM service. Returns a descriptor for Java to pump the
// phone's bytes through, which it then owns, or -1 to have the phone dropped. Called on
// Java's accept thread.
using RfcommHandler = std::function<int(const std::string& address)>;

// Publishes an RFCOMM service under `uuid` and hands every phone that connects to
// `handler`. Returns an empty string, or why it could not.
std::string StartRfcomm(const std::string& uuid, const std::string& name,
                        RfcommHandler handler);

// Withdraws the service and drops the phones on it. Once this returns the handler is
// not called again.
void StopRfcomm();

// The bonded Bluetooth devices, one per line, tab separated: address, name, "1" if
// connected, "1" if a phone.
std::string PairedDevices();

// The phone whose Bluetooth link came up last, or empty.
std::string ConnectedPhone();

// Opens a headset link to `address`, which is what gets a phone to dial a head unit
// that has no hands free profile of its own. Returns at once.
void NudgePhone(const std::string& address);

// The Wi-Fi network this device is on, tab separated: interface, SSID, BSSID, IPv4,
// frequency in MHz or 0.
std::string WifiNetwork();

// Brings up a LocalOnlyHotspot if it is not up, and describes it, tab separated:
// interface, SSID, BSSID, IPv4, passphrase, frequency in MHz or 0. Empty if it would
// not start. Blocks for up
// to a few seconds the first time.
std::string HostWifiNetwork();

// Takes the hotspot down.
void ReleaseWifiNetwork();

}  // namespace android
}  // namespace aa

#endif  // defined(__ANDROID__)

#endif  // ANDROID_AUTO_PLATFORM_ANDROID_JNI_H_
