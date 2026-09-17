// SPDX-License-Identifier: GPL-3.0-or-later
// Gets a phone projecting without a cable, and hands the result to the same session
// the USB path uses.
//
// Three things have to line up, in this order:
//
//   1. Bluetooth. The head unit publishes an RFCOMM service and the phone opens it.
//   2. The AAW handshake on that channel. The head unit says which Wi-Fi network to be
//      on and which address to dial.
//   3. TCP. The phone joins the network and connects to port 5288, and from there the
//      protocol is byte for byte the one that runs over the cable.
//
// Only the first two are wireless specific. The third produces an aasdk transport and
// everything above it, SSL included, is shared with USB, which is why this file ends
// at a transport rather than knowing anything about channels.
//
// The counterpart of UsbConnector, and deliberately the same shape: Start installs
// handlers, Stop only asks, and the object outlives the sessions built on what it
// hands over. What it does not share is the recovery: there is no device to bounce and
// no accessory mode to leave, so losing the link means re-arming the acceptor and
// waiting, which is much less than the USB path has to do.

#ifndef ANDROID_AUTO_LINUX_SESSION_WIRELESS_CONNECTOR_H_
#define ANDROID_AUTO_LINUX_SESSION_WIRELESS_CONNECTOR_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Transport/ITransport.hpp>

#include "../bluetooth/bluez_client.h"
#include "../wireless/aaw_handshake.h"

namespace aa {

// What the host app asked for. Every string may be empty, and an empty one means "work
// it out", which is what makes a head unit on an ordinary Wi-Fi network need no
// configuration beyond the passphrase.
struct WirelessSettings {
  std::string ssid;
  std::string passphrase;
  std::string bssid;
  std::string interface;
  std::string ip;
  // Which paired phone to prod when nothing has happened. Empty leaves it to the
  // phone, which is the normal way round.
  std::string phone_address;
  uint16_t port = 5288;
  // A WifiSecurityMode, 5 being WPA2 personal.
  int32_t security = 5;
  // An AccessPointType, or -1 to decide from whether this machine is hosting the
  // network rather than joined to one.
  int32_t access_point = -1;

  bool operator==(const WirelessSettings& other) const {
    return ssid == other.ssid && passphrase == other.passphrase &&
           bssid == other.bssid && interface == other.interface && ip == other.ip &&
           phone_address == other.phone_address && port == other.port &&
           security == other.security && access_point == other.access_point;
  }
  bool operator!=(const WirelessSettings& other) const { return !(*this == other); }
};

// What it resolved to, for a host app that wants to show the driver why nothing is
// happening. Every field is filled in by Start, whether it came from the settings or
// from the machine.
struct WirelessSummary {
  std::string interface;
  std::string ssid;
  std::string bssid;
  std::string ip;
  uint16_t port = 0;
  bool hosting = false;
  bool bluetooth_ready = false;
  bool phone_linked = false;
};

class WirelessConnector {
 public:
  // Hands over a live transport to a phone that has dialled in. `peer` is its address,
  // for the log. Called on an io_context thread.
  using TransportHandler =
      std::function<void(aasdk::transport::ITransport::Pointer, std::string peer)>;
  using StateHandler = std::function<void(int state, const std::string& message)>;

  explicit WirelessConnector(boost::asio::io_context& io_context);
  ~WirelessConnector();

  WirelessConnector(const WirelessConnector&) = delete;
  WirelessConnector& operator=(const WirelessConnector&) = delete;

  // Publishes the Bluetooth service and opens the TCP port. Returns an error string on
  // failure, empty on success.
  std::string Start(const WirelessSettings& settings, TransportHandler on_transport,
                    StateHandler on_state);

  // Stops offering wireless without taking the Bluetooth service down.
  //
  // The service stays published and every connection is answered with a refusal. That
  // is deliberately not the same as Stop: a phone that has been introduced to this
  // machine as a wireless car asks for the service every five seconds for as long as
  // it is connected over Bluetooth, and withdrawing the service does not stop it
  // asking, it only stops it getting an answer. The driver then has a notification
  // saying the phone is connecting, permanently, while nothing is. Refusing makes it
  // give up. See DeclineWirelessOnSocket.
  //
  // This is what a stopped session should leave behind, rather than silence.
  void Decline();

  // Takes the service down and closes the port. For shutting the whole plugin down;
  // a head unit that is merely not projecting wants Decline.
  void Stop();

  // Drops the handlers. The connector outlives the session that installed them, the
  // same rule UsbConnector::ClearHandlers exists for.
  void ClearHandlers();

  // Listens again after a connection ended. The Bluetooth channel usually survives a
  // Wi-Fi blip, so a phone that comes back finds the port already open.
  void Rearm();

  bool running() const { return running_.load(); }
  WirelessSummary summary() const;

  // What it was started with, so a caller can tell whether a restart would change
  // anything. Only meaningful while running.
  WirelessSettings settings() const;

 private:
  void Accept();
  // Arms the grace period after which a phone that has said nothing is prodded, see
  // the note in the .cc file.
  void ArmNudge();
  void OnBluetoothConnection(int fd, const std::string& address);
  void Report(int state, const std::string& message);
  // Arms AA_WIRELESS_FAKE_PHONE, see the note in the .cc file.
  void ListenForFakePhone();
  void AcceptFakePhone();

  boost::asio::io_context& io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::steady_timer nudge_timer_;
  // Whether any phone has opened the Bluetooth channel since this started offering.
  // Written on BlueZ's thread, read on an io thread.
  std::atomic<bool> asked_since_offering_{false};
  // The last phone that opened a channel, so it can be prodded without the host app
  // having named one.
  std::string last_phone_;

  BluezClient bluetooth_;
  // A stand in for BlueZ, for testing without a phone. Null unless
  // AA_WIRELESS_FAKE_PHONE is set.
  std::unique_ptr<boost::asio::local::stream_protocol::acceptor> fake_phone_;
  // The live Bluetooth conversation, or nullptr. Written from BlueZ's own thread and
  // read from io threads and from Flutter's platform thread, hence the mutex.
  mutable std::mutex handshake_mutex_;
  AawHandshake::Pointer handshake_;

  WirelessOffer offer_;
  WirelessSettings settings_;
  mutable std::mutex summary_mutex_;
  WirelessSummary summary_;

  TransportHandler on_transport_;
  StateHandler on_state_;

  std::atomic<bool> running_{false};
  // Whether to refuse rather than offer. Read on BlueZ's thread.
  std::atomic<bool> declining_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_WIRELESS_CONNECTOR_H_
