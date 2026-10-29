// SPDX-License-Identifier: GPL-3.0-or-later
// Reading the Wi-Fi network on Android. The same three functions as wifi_network.cc.
//
// The wireless extensions that file asks are closed to apps on Android, so the SSID,
// BSSID and address come from WifiManager through Java instead. Addresses alone, which
// are re-read during a session, come from getifaddrs, which apps may still call.
//
// A network of the head unit's own is a LocalOnlyHotspot, brought up by Java, which
// chooses its own SSID and passphrase and says what they are.

#include "wifi_network.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstdlib>
#include <sstream>

#include "../platform/android_jni.h"

namespace aa {
namespace {

// The first IPv4 address, on `interface` if it is not empty. Wireless interfaces first
// when it is, since that is where a phone can reach.
std::string FindAddress(const std::string& interface) {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    return std::string();
  }
  std::string wireless;
  std::string other;
  for (ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
        (entry->ifa_flags & IFF_LOOPBACK) != 0 || (entry->ifa_flags & IFF_UP) == 0) {
      continue;
    }
    const std::string name = entry->ifa_name == nullptr ? "" : entry->ifa_name;
    if (!interface.empty() && name != interface) {
      continue;
    }
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr, text,
              sizeof(text));
    if (name.rfind("wlan", 0) == 0) {
      if (wireless.empty()) wireless = text;
    } else if (other.empty()) {
      other = text;
    }
  }
  freeifaddrs(list);
  return wireless.empty() ? other : wireless;
}

}  // namespace

WifiNetwork DetectWifiNetwork(const std::string& interface) {
  WifiNetwork network;
  std::istringstream fields(android::WifiNetwork());
  std::getline(fields, network.interface, '\t');
  std::getline(fields, network.ssid, '\t');
  std::getline(fields, network.bssid, '\t');
  std::getline(fields, network.ip, '\t');
  std::string frequency;
  std::getline(fields, frequency, '\t');
  network.frequency_mhz = std::atoi(frequency.c_str());
  if (!interface.empty() && interface != network.interface) {
    // The host app named another interface. Its address is still worth reading; the
    // rest of what Java said was about a different one.
    network = WifiNetwork{};
    network.interface = interface;
    network.ip = FindAddress(interface);
  }
  if (network.ip.empty() && !network.interface.empty()) {
    network.ip = FindAddress(network.interface);
  }
  return network;
}

std::string InterfaceAddress(const std::string& interface) {
  return interface.empty() ? std::string() : FindAddress(interface);
}

std::string PrimaryAddress() { return FindAddress(std::string()); }

WifiNetwork HostWifiNetwork() {
  WifiNetwork network;
  std::istringstream fields(android::HostWifiNetwork());
  std::getline(fields, network.interface, '\t');
  std::getline(fields, network.ssid, '\t');
  std::getline(fields, network.bssid, '\t');
  std::getline(fields, network.ip, '\t');
  std::getline(fields, network.passphrase, '\t');
  std::string frequency;
  std::getline(fields, frequency, '\t');
  network.frequency_mhz = std::atoi(frequency.c_str());
  network.hosting = !network.ssid.empty();
  if (network.ip.empty() && !network.interface.empty()) {
    network.ip = FindAddress(network.interface);
  }
  return network;
}

void ReleaseWifiNetwork() { android::ReleaseWifiNetwork(); }

}  // namespace aa
