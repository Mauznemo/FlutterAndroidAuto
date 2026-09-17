// SPDX-License-Identifier: GPL-3.0-or-later
// What network the head unit is going to tell the phone to use.
//
// The Bluetooth handshake hands the phone an SSID, a passphrase, a BSSID and an
// address to connect back to. Three of those four can be read off the machine, and the
// fourth cannot: nothing here can discover a passphrase, because the kernel does not
// keep one and the network manager's copy is behind a privileged interface. So the
// host app supplies the passphrase and this fills in the rest, which is the split that
// makes a head unit work out of the box on the network it is already on.
//
// Nothing in here configures anything. Bringing up an access point, or joining a
// network, is the machine's business and not a plugin's, exactly as the echo canceller
// for phone calls is configuration rather than code.

#ifndef ANDROID_AUTO_LINUX_WIRELESS_WIFI_NETWORK_H_
#define ANDROID_AUTO_LINUX_WIRELESS_WIFI_NETWORK_H_

#include <string>

namespace aa {

// How the phone should be told to reach the head unit.
struct WifiNetwork {
  // The wireless interface this describes, "wlp0s20f3" and the like. Empty when the
  // machine has no wireless interface at all, which is the one case that cannot be
  // worked around by the host app filling fields in.
  std::string interface;
  std::string ssid;
  // The access point's MAC. In access point mode that is this machine's own radio.
  std::string bssid;
  // IPv4 the phone will connect back to. Read from the interface, so it is the address
  // on the wireless network rather than whatever a default route points at.
  std::string ip;
  // True when the interface is running as an access point rather than joined to
  // someone else's network. Worth knowing because the two cases fail differently: an
  // access point that nobody joins is a configuration problem on this machine, and a
  // joined network the phone cannot see is a problem at the phone's end.
  bool hosting = false;
};

// Reads the current state of `interface`, or of the first wireless interface that has
// an address when `interface` is empty. Every field it cannot read is left empty
// rather than guessed.
WifiNetwork DetectWifiNetwork(const std::string& interface);

// The IPv4 address of `interface`, or empty. Split out because the address is the one
// field that changes while a head unit is running, when a lease is renewed or an
// access point is brought up after the app started.
std::string InterfaceAddress(const std::string& interface);

// The address the machine reaches the world on: the IPv4 of whichever interface
// carries the default route, falling back to the first that is not loopback.
//
// For the arrangement where the head unit is wired and the phone is on the house
// Wi-Fi. The two are on one subnet, so the phone can reach the machine perfectly
// well, but the machine's wireless interface has no address to offer and reading one
// off it would hand the phone nothing. Only the SSID has to be supplied by hand then,
// because a machine on Ethernet genuinely cannot know what the phone is connected to.
std::string PrimaryAddress();

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_WIRELESS_WIFI_NETWORK_H_
