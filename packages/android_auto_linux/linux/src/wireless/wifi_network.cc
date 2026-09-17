#include "wifi_network.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/wireless.h>

#include <cstdio>
#include <vector>

namespace aa {
namespace {

// The wireless extensions ioctls, not netlink.
//
// They are the old interface and nothing new should be built on them, but reading an
// SSID and a BSSID is exactly what cfg80211 still answers through the compatibility
// layer, and the alternative is nl80211, which means either libnl as a new dependency
// or several hundred lines of hand rolled netlink for two strings. Every field here is
// read only; nothing is configured through this.
//
// A driver that answers neither leaves the fields empty, which the host app can fill
// in itself, so this degrades rather than fails.

std::string WirelessString(const std::string& interface, unsigned long request) {
  if (interface.empty() || interface.size() >= IFNAMSIZ) {
    return {};
  }
  const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return {};
  }
  struct iwreq query;
  memset(&query, 0, sizeof(query));
  snprintf(query.ifr_name, IFNAMSIZ, "%s", interface.c_str());

  std::string value;
  if (request == SIOCGIWESSID) {
    char essid[IW_ESSID_MAX_SIZE + 1];
    memset(essid, 0, sizeof(essid));
    query.u.essid.pointer = essid;
    query.u.essid.length = IW_ESSID_MAX_SIZE;
    if (ioctl(socket_fd, SIOCGIWESSID, &query) >= 0) {
      value.assign(essid);
    }
  } else if (request == SIOCGIWAP) {
    if (ioctl(socket_fd, SIOCGIWAP, &query) >= 0) {
      const auto* mac = reinterpret_cast<const unsigned char*>(query.u.ap_addr.sa_data);
      char text[18];
      snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
      value.assign(text);
      // An interface that is up but associated with nothing answers all zeroes, which
      // is not a BSSID and would be worse than an empty field on the wire.
      if (value == "00:00:00:00:00:00") {
        value.clear();
      }
    }
  }
  close(socket_fd);
  return value;
}

// The interface's own MAC, which in access point mode is the BSSID.
//
// Needed because the wireless extensions cannot answer for an access point. The
// kernel's compatibility layer implements SIOCGIWAP for station and ad hoc
// interfaces and returns EINVAL for everything else, so a head unit hosting the
// network it is telling the phone to join reads nothing at all, and an empty BSSID
// is not a BSSID. What that costs is invisible from this end: the phone rejects the
// details outright, without scanning and without associating, and reports it as
// incorrect credentials. Confirmed by the absence of any association attempt in
// wpa_supplicant's log over forty seconds with the access point up.
std::string InterfaceMac(const std::string& interface) {
  if (interface.empty() || interface.size() >= IFNAMSIZ) {
    return {};
  }
  const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return {};
  }
  struct ifreq request;
  memset(&request, 0, sizeof(request));
  snprintf(request.ifr_name, IFNAMSIZ, "%s", interface.c_str());
  std::string value;
  if (ioctl(socket_fd, SIOCGIFHWADDR, &request) >= 0) {
    const auto* mac =
        reinterpret_cast<const unsigned char*>(request.ifr_hwaddr.sa_data);
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    value.assign(text);
  }
  close(socket_fd);
  return value;
}

bool IsHostingAccessPoint(const std::string& interface) {
  if (interface.empty() || interface.size() >= IFNAMSIZ) {
    return false;
  }
  const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return false;
  }
  struct iwreq query;
  memset(&query, 0, sizeof(query));
  snprintf(query.ifr_name, IFNAMSIZ, "%s", interface.c_str());
  bool hosting = false;
  if (ioctl(socket_fd, SIOCGIWMODE, &query) >= 0) {
    hosting = query.u.mode == IW_MODE_MASTER;
  }
  close(socket_fd);
  return hosting;
}

// A network interface is wireless if the kernel gave it a wireless directory. Cheaper
// and more reliable than probing every interface with an ioctl that logs on failure.
bool IsWireless(const std::string& interface) {
  const std::string path = "/sys/class/net/" + interface + "/wireless";
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::vector<std::string> WirelessInterfaces() {
  std::vector<std::string> names;
  DIR* directory = opendir("/sys/class/net");
  if (directory == nullptr) {
    return names;
  }
  while (const struct dirent* entry = readdir(directory)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (IsWireless(name)) {
      names.push_back(name);
    }
  }
  closedir(directory);
  return names;
}

}  // namespace

std::string InterfaceAddress(const std::string& interface) {
  if (interface.empty()) {
    return {};
  }
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    return {};
  }
  std::string address;
  for (struct ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if (interface != entry->ifa_name) {
      continue;
    }
    char text[INET_ADDRSTRLEN] = {0};
    const auto* in = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
    if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr) {
      address.assign(text);
      break;
    }
  }
  freeifaddrs(list);
  return address;
}

std::string PrimaryAddress() {
  // The default route's interface, straight out of /proc/net/route: destination zero
  // with the gateway flag set. Cheaper than a netlink round trip and it needs no
  // library.
  std::string route_interface;
  if (FILE* routes = fopen("/proc/net/route", "r")) {
    char name[IFNAMSIZ + 1];
    unsigned long destination = 0;
    unsigned long flags = 0;
    char line[512];
    // Skip the header.
    if (fgets(line, sizeof(line), routes) != nullptr) {
      while (fgets(line, sizeof(line), routes) != nullptr) {
        unsigned long gateway = 0;
        if (sscanf(line, "%16s %lx %lx %lx", name, &destination, &gateway, &flags) ==
                4 &&
            destination == 0) {
          route_interface.assign(name);
          break;
        }
      }
    }
    fclose(routes);
  }
  if (!route_interface.empty()) {
    const std::string address = InterfaceAddress(route_interface);
    if (!address.empty()) {
      return address;
    }
  }

  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    return {};
  }
  std::string address;
  for (struct ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    char text[INET_ADDRSTRLEN] = {0};
    const auto* in = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
    if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr) {
      address.assign(text);
      break;
    }
  }
  freeifaddrs(list);
  return address;
}

WifiNetwork DetectWifiNetwork(const std::string& interface) {
  WifiNetwork network;

  if (!interface.empty()) {
    network.interface = interface;
  } else {
    // Prefer an interface that actually has an address: a machine with two radios,
    // one of them idle, would otherwise hand the phone an address of nothing.
    for (const std::string& candidate : WirelessInterfaces()) {
      if (network.interface.empty()) {
        network.interface = candidate;
      }
      if (!InterfaceAddress(candidate).empty()) {
        network.interface = candidate;
        break;
      }
    }
  }
  if (network.interface.empty()) {
    return network;
  }

  network.ssid = WirelessString(network.interface, SIOCGIWESSID);
  network.bssid = WirelessString(network.interface, SIOCGIWAP);
  network.ip = InterfaceAddress(network.interface);
  network.hosting = IsHostingAccessPoint(network.interface);

  // Hosting means this machine is the access point, so this machine's MAC is the
  // BSSID. Only in that case: falling back to it for a station interface that is not
  // associated would hand the phone this machine's address as the address of an
  // access point somewhere else, which is worse than handing it nothing.
  if (network.hosting && network.bssid.empty()) {
    network.bssid = InterfaceMac(network.interface);
  }
  return network;
}

}  // namespace aa
