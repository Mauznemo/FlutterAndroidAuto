// SPDX-License-Identifier: GPL-3.0-or-later
// BluezClient on Android, which has no BlueZ: the same interface over Java's Bluetooth.
//
// Android's Bluetooth stack is only reachable from Java, so the RFCOMM server is a
// BluetoothServerSocket in AndroidAutoPlugin.java and this is the native end of it.
// Java hands no descriptor over for a BluetoothSocket, so each phone that connects gets
// a socket pair: one end goes to the wireless connector exactly as BlueZ's RFCOMM
// descriptor would on Linux, and Java copies bytes between the other end and the phone.
//
// Two things BlueZ does that Android does not offer an app: saying whether a bonded
// device is connected right now, and dropping and remaking a phone's link. So the
// paired list reports every phone as not connected, ConnectedPhone goes by link
// broadcasts, and Reconnect opens a headset link instead, see nudgePhone in Java.

#include "bluez_client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <sstream>

#include "../platform/android_jni.h"

namespace aa {

const char kAndroidAutoWirelessUuid[] = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

BluezClient::BluezClient() = default;

BluezClient::~BluezClient() { Stop(); }

std::vector<BluetoothDevice> BluezClient::PairedDevices(std::string* error) {
  std::vector<BluetoothDevice> devices;
  std::istringstream lines(android::PairedDevices());
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    BluetoothDevice device;
    std::string connected;
    std::string phone;
    std::getline(fields, device.address, '\t');
    std::getline(fields, device.name, '\t');
    std::getline(fields, connected, '\t');
    std::getline(fields, phone, '\t');
    if (device.address.empty()) {
      continue;
    }
    device.paired = true;
    device.connected = connected == "1";
    device.is_phone = phone == "1";
    devices.push_back(std::move(device));
  }
  if (error != nullptr) {
    error->clear();
  }
  return devices;
}

std::string BluezClient::Start(const ProfileOptions& options, ConnectionHandler on_connection,
                               ErrorHandler on_error) {
  if (started_.load()) {
    return std::string();
  }
  on_connection_ = std::move(on_connection);
  on_error_ = std::move(on_error);
  const std::string error =
      android::StartRfcomm(options.uuid, options.name, [this](const std::string& address) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
          return -1;
        }
        if (!on_connection_) {
          close(pair[0]);
          close(pair[1]);
          return -1;
        }
        // Ownership of pair[0] passes to the connector, of pair[1] to Java.
        on_connection_(pair[0], address);
        return pair[1];
      });
  if (!error.empty()) {
    return error;
  }
  // Android picks the channel and writes it into the SDP record itself; nothing here
  // ever needs the number, so a nonzero stand in says "published".
  channel_ = 1;
  started_ = true;
  return std::string();
}

void BluezClient::Stop() {
  if (!started_.exchange(false)) {
    return;
  }
  android::StopRfcomm();
  channel_ = 0;
}

void BluezClient::Reconnect(const std::string& address) {
  // An app cannot drop the system's Bluetooth link to a phone. What it can do is open a
  // headset link to it, which is what makes a phone dial a head unit that, unlike a car,
  // offers no hands free profile of its own.
  android::NudgePhone(address);
}

std::string BluezClient::ConnectedPhone() const { return android::ConnectedPhone(); }

}  // namespace aa
