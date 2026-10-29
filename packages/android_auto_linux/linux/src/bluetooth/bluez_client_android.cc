// SPDX-License-Identifier: GPL-3.0-or-later
// BluezClient on Android, which has no BlueZ to talk to.
//
// Android's Bluetooth stack is only reachable from Java, through BluetoothAdapter and
// a BluetoothServerSocket for the RFCOMM service. Until that is wired through JNI,
// wireless projection reports that Bluetooth is unavailable here, which is exactly what
// it reports on a Linux machine with no adapter. Every other transport is unaffected.

#include "bluez_client.h"

namespace aa {

const char kAndroidAutoWirelessUuid[] = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

BluezClient::BluezClient() = default;

BluezClient::~BluezClient() { Stop(); }

std::vector<BluetoothDevice> BluezClient::PairedDevices(std::string* error) {
  if (error != nullptr) {
    *error = "Bluetooth is not implemented on Android yet.";
  }
  return {};
}

std::string BluezClient::Start(const ProfileOptions&, ConnectionHandler, ErrorHandler) {
  return "Bluetooth is not implemented on Android yet, so wireless Android Auto cannot "
         "start.";
}

void BluezClient::Stop() { started_ = false; }

void BluezClient::Reconnect(const std::string&) {}

std::string BluezClient::ConnectedPhone() const { return std::string(); }

}  // namespace aa
