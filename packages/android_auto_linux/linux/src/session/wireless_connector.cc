#include "wireless_connector.h"

#include <aasdk/Common/Log.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include <cstdlib>
#include <cstdio>

#include "../aa_core.h"
#include "../wireless/wifi_network.h"

namespace aa {
namespace {

// aasdk's TCPEndpoint keeps this by reference and is destroyed by a reference count
// dropping on an io thread, long after whoever built it has gone. The object holds no
// state at all, so the same answer the libusb context got applies here for a much
// smaller price: one of them, for the life of the process.
aasdk::tcp::TCPWrapper& SharedTcpWrapper() {
  static aasdk::tcp::TCPWrapper wrapper;
  return wrapper;
}

}  // namespace

WirelessConnector::WirelessConnector(boost::asio::io_context& io_context)
    : io_context_(io_context), acceptor_(io_context), nudge_timer_(io_context) {}

WirelessConnector::~WirelessConnector() { Stop(); }

std::string WirelessConnector::Start(const WirelessSettings& settings,
                                     TransportHandler on_transport,
                                     StateHandler on_state) {
  if (running_.load()) {
    return {};
  }
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    settings_ = settings;
  }
  on_transport_ = std::move(on_transport);
  on_state_ = std::move(on_state);

  // What the machine says about itself, then whatever the host app overrode. The
  // passphrase is the one field that can only come from the host app: nothing here can
  // read one, see wifi_network.h.
  const WifiNetwork network = DetectWifiNetwork(settings.interface);
  WirelessSummary resolved;
  resolved.interface =
      settings.interface.empty() ? network.interface : settings.interface;
  resolved.ssid = settings.ssid.empty() ? network.ssid : settings.ssid;
  resolved.bssid = settings.bssid.empty() ? network.bssid : settings.bssid;
  resolved.ip = settings.ip.empty() ? network.ip : settings.ip;
  if (resolved.ip.empty()) {
    // No address on the wireless interface, which is what a head unit wired to the
    // network looks like. The phone is on the house Wi-Fi and this machine is on the
    // same subnet over Ethernet, so the address it can be reached on is the one it
    // reaches the world on. The SSID still has to be supplied by hand: a machine on
    // Ethernet cannot know what the phone is connected to.
    resolved.ip = PrimaryAddress();
  }
  resolved.port = settings.port == 0 ? 5288 : settings.port;
  resolved.hosting = network.hosting;

  if (resolved.ssid.empty()) {
    return "No Wi-Fi network to offer the phone. This machine is not on one, and the "
           "host app named none. A head unit wired to the network has to be told the "
           "name of the Wi-Fi the phone is on.";
  }
  if (resolved.ip.empty()) {
    return "This machine has no address on any network, so there is nothing to tell "
           "the phone to connect to.";
  }
  if (resolved.bssid.empty()) {
    // Worth stopping for rather than trying anyway. A phone given an offer with no
    // BSSID rejects it without scanning and without associating, and calls that
    // incorrect credentials, which sends whoever is debugging it to look at the
    // passphrase for as long as they can stand.
    //
    // Only reachable when this machine is neither hosting the network nor joined to
    // it, which is the head unit wired to the LAN with the phone on the house Wi-Fi.
    // It cannot read the MAC of an access point it is not talking to, so it has to be
    // told, the same way it has to be told the SSID in that arrangement.
    return "No BSSID to give the phone for " + resolved.ssid +
           ". A phone rejects an offer without one and reports it as a bad "
           "passphrase, so this stops here rather than sending you after the "
           "passphrase. Name the access point's MAC in the wireless configuration.";
  }
  // An open network is a real configuration and a passphrase would be wrong on one, so
  // this is a warning in the log rather than a refusal.
  if (settings.passphrase.empty() && settings.security != 1) {
    AASDK_LOG(warning) << "[Wireless] no Wi-Fi passphrase was given, so the phone will "
                          "be told to join " << resolved.ssid << " with an empty one";
  }

  offer_.ssid = resolved.ssid;
  offer_.passphrase = settings.passphrase;
  offer_.bssid = resolved.bssid;
  offer_.ip = resolved.ip;
  offer_.port = resolved.port;
  offer_.security = settings.security;
  // Dynamic means the head unit brought this network up for the phone, static means it
  // was already there. Which one is true is something the machine can answer.
  offer_.access_point = settings.access_point >= 0 ? settings.access_point
                                                   : (network.hosting ? 1 : 0);

  boost::system::error_code ec;
  const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                                resolved.port);
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) {
    return "Could not open the projection port: " + ec.message();
  }
  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  acceptor_.bind(endpoint, ec);
  if (ec) {
    acceptor_.close(ec);
    return "Could not bind port " + std::to_string(resolved.port) + ": " + ec.message();
  }
  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) {
    acceptor_.close(ec);
    return "Could not listen on port " + std::to_string(resolved.port) + ": " +
           ec.message();
  }

  const std::string bluetooth_error = bluetooth_.Start(
      ProfileOptions{},
      [this](int fd, std::string address) { OnBluetoothConnection(fd, address); },
      [this](std::string message) { Report(AA_STATE_ERROR, message); });
  if (!bluetooth_error.empty()) {
    acceptor_.close(ec);
    return bluetooth_error;
  }

  resolved.bluetooth_ready = true;
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    summary_ = resolved;
  }


  running_ = true;
  declining_ = false;
  asked_since_offering_ = false;
  Accept();
  ListenForFakePhone();

  // Every field the phone is about to be judged on, in one line. A phone that
  // dislikes any of them says only "incorrect credentials" and does not say which,
  // so the log has to carry the whole offer or the next failure is unreadable.
  AASDK_LOG(info) << "[Wireless] ready on " << resolved.interface << ", offering ssid "
                  << resolved.ssid << ", bssid "
                  << (resolved.bssid.empty() ? std::string("(none)") : resolved.bssid)
                  << ", security " << offer_.security << ", access point type "
                  << offer_.access_point << ", passphrase "
                  << (offer_.passphrase.empty() ? "(none)" : "set") << ", at "
                  << resolved.ip << ":" << resolved.port
                  << (resolved.hosting ? " (hosting)" : " (joined)");
  Report(AA_STATE_SEARCHING,
         "Waiting for a phone over Wi-Fi. Offering " + resolved.ssid + " and " +
             resolved.ip + ":" + std::to_string(resolved.port) + ".");

  ArmNudge();
  return {};
}

void WirelessConnector::OnBluetoothConnection(int fd, const std::string& address) {
  // Runs on BlueZ's own thread. Everything it starts runs on the io_context.
  asked_since_offering_ = true;
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    last_phone_ = address;
  }
  if (declining_.load()) {
    AASDK_LOG(info) << "[Wireless] " << address
                    << " asked to project and wireless is not on offer";
    DeclineWirelessOnSocket(fd);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(handshake_mutex_);
    if (handshake_) {
      // A second phone, or the same one reconnecting. The newest wins: the old channel
      // is one the phone has already given up on.
      handshake_->Stop();
      handshake_.reset();
    }
  }

  // The address can have changed since Start, when a lease was renewed or the machine
  // joined a different network, and handing the phone a stale one is a connection that
  // times out with nothing in any log to say why.
  WirelessOffer offer = offer_;
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    const std::string current = InterfaceAddress(summary_.interface);
    if (!current.empty() && current != summary_.ip) {
      AASDK_LOG(info) << "[Wireless] address changed from " << summary_.ip << " to "
                      << current;
      summary_.ip = current;
    }
    offer.ip = summary_.ip;
  }

  auto handshake = AawHandshake::Create(
      io_context_, fd, std::move(offer),
      [this](std::string message) { Report(AA_STATE_SEARCHING, message); },
      [this](std::string message) { Report(AA_STATE_ERROR, message); });
  {
    std::lock_guard<std::mutex> lock(handshake_mutex_);
    handshake_ = handshake;
  }
  // Logged here rather than only in BluezClient, so a stand in reports the same stage
  // a real phone does and nothing reading the log can be fooled by the difference.
  AASDK_LOG(info) << "[Wireless] " << address << " opened the Bluetooth channel";
  Report(AA_STATE_SEARCHING, "Phone " + address + " connected over Bluetooth.");
  handshake->Start();
}

// Lets something other than a phone play the phone's Bluetooth part.
//
// Everything in the wireless path except the phone itself can be exercised on one
// machine: the message framing, the order the two sides speak in, the addresses that
// end up on the wire, and then the whole projection stack behind the TCP acceptor. The
// only thing in the way is that an RFCOMM socket can only come from BlueZ and a paired
// phone that has decided to project.
//
// So this takes a socket from somewhere else. A Unix socket at the path named in
// AA_WIRELESS_FAKE_PHONE is listened on, and a connection to it is handed to exactly
// the code an RFCOMM connection would reach, because by that point a socket is a
// socket. Whatever connects then speaks the message framing in wireless/aaw_handshake
// and dials the address it is given.
//
// Unset, this costs one getenv per start. It cannot be reached from Dart and there is
// deliberately no way to turn it on from the host app.
void WirelessConnector::ListenForFakePhone() {
  const char* path = std::getenv("AA_WIRELESS_FAKE_PHONE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  ::remove(path);
  // Opened, bound and listened in three steps taking an error code, the same way the
  // projection acceptor above is. The one argument constructor that does all three
  // throws instead, and a bind failure here would leave an io thread by exception.
  boost::system::error_code ec;
  const boost::asio::local::stream_protocol::endpoint endpoint(path);
  fake_phone_ =
      std::make_unique<boost::asio::local::stream_protocol::acceptor>(io_context_);
  fake_phone_->open(endpoint.protocol(), ec);
  if (!ec) {
    fake_phone_->bind(endpoint, ec);
  }
  if (!ec) {
    fake_phone_->listen(boost::asio::socket_base::max_listen_connections, ec);
  }
  if (ec) {
    AASDK_LOG(warning) << "[Wireless] could not open the fake phone socket at " << path
                       << ": " << ec.message();
    fake_phone_.reset();
    return;
  }
  AASDK_LOG(info) << "[Wireless] standing in for BlueZ on " << path;
  AcceptFakePhone();
}

void WirelessConnector::AcceptFakePhone() {
  if (!fake_phone_) {
    return;
  }
  auto socket =
      std::make_shared<boost::asio::local::stream_protocol::socket>(io_context_);
  fake_phone_->async_accept(*socket, [this, socket](
                                         const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    // release() hands the descriptor over without closing it, which is the same deal
    // BlueZ's NewConnection gives.
    boost::system::error_code release_error;
    const int fd = socket->release(release_error);
    if (!release_error) {
      OnBluetoothConnection(fd, "fake phone");
    }
    AcceptFakePhone();
  });
}

// Prods the phone if it has not asked for a while, and only then.
//
// Refusing works, which is the problem this solves. A phone that has been told twice
// that wireless is not on offer stops asking, and it does not start again when the
// offer changes: the head unit can sit there advertising to a phone that has stopped
// listening, which is exactly what pressing start after a spell of not projecting
// looked like. What makes a phone look again is the Bluetooth link coming up, so that
// is what gets done to it.
//
// Behind a grace period because it costs the phone's Bluetooth audio a few seconds. A
// phone that has not been refused recently is still asking every five seconds anyway,
// so most of the time this timer fires having already been cancelled by an arrival and
// nothing is disturbed.
void WirelessConnector::ArmNudge() {
  nudge_timer_.expires_after(std::chrono::seconds(8));
  nudge_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (ec || !running_.load() || declining_.load()) {
      return;
    }
    if (asked_since_offering_.load()) {
      return;
    }
    std::string address;
    {
      std::lock_guard<std::mutex> lock(summary_mutex_);
      address = settings_.phone_address.empty() ? last_phone_
                                                : settings_.phone_address;
    }
    if (address.empty()) {
      address = bluetooth_.ConnectedPhone();
    }
    if (address.empty()) {
      AASDK_LOG(info) << "[Wireless] no phone is connected over Bluetooth, so there "
                         "is nobody to tell that wireless is on offer";
      return;
    }
    AASDK_LOG(info) << "[Wireless] " << address
                    << " has not asked since wireless came back on offer, prodding it";
    bluetooth_.Reconnect(address);
  });
}

void WirelessConnector::Accept() {
  if (!running_.load() || !acceptor_.is_open()) {
    return;
  }
  auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context_);
  acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (ec != boost::asio::error::operation_aborted && running_.load()) {
        Report(AA_STATE_ERROR, "Could not accept the phone's connection: " +
                                   ec.message());
        Accept();
      }
      return;
    }

    boost::system::error_code option_error;
    // Projection is a stream of small acknowledgements interleaved with large video
    // frames. Nagle would hold the acknowledgements back waiting for company, which
    // is exactly the wrong trade for a link the phone paces itself against.
    socket->set_option(boost::asio::ip::tcp::no_delay(true), option_error);

    const std::string peer =
        socket->remote_endpoint(option_error).address().to_string();
    AASDK_LOG(info) << "[Wireless] phone dialled in from " << peer;

    auto endpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(SharedTcpWrapper(), socket);
    auto transport =
        std::make_shared<aasdk::transport::TCPTransport>(io_context_, endpoint);

    // The Bluetooth side has to know, or it will keep telling a phone that is
    // already connected to connect.
    {
      std::lock_guard<std::mutex> lock(handshake_mutex_);
      if (handshake_) {
        handshake_->NotifyProjecting();
      }
    }

    if (on_transport_) {
      on_transport_(transport, peer);
    }
    // Deliberately not accepting again here. One phone projects at a time, and a
    // second connection arriving mid session would be handed to a session that is
    // already busy. Rearm() is what opens the door again once this one has ended.
  });
}

void WirelessConnector::Rearm() {
  if (!running_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(handshake_mutex_);
    if (handshake_) {
      handshake_->NotifyDisconnected();
    }
  }
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    const std::string current = InterfaceAddress(summary_.interface);
    if (!current.empty()) {
      summary_.ip = current;
    }
  }
  Accept();
}

void WirelessConnector::Decline() {
  declining_ = true;
  nudge_timer_.cancel();
  if (!bluetooth_.started()) {
    // Publish the service purely so that it can say no.
    //
    // This is the state a head unit spends most of its life in: switched on, not
    // projecting. A phone that knows the machine asks every five seconds regardless,
    // so the choice is between answering and not, and not answering is what leaves
    // the driver with a permanent notification. Publishing needs no network, no
    // io_context and no configuration: refusing is one write on the socket BlueZ
    // hands over.
    const std::string error = bluetooth_.Start(
        ProfileOptions{},
        [this](int fd, std::string address) { OnBluetoothConnection(fd, address); },
        [this](std::string message) { Report(AA_STATE_ERROR, message); });
    if (!error.empty()) {
      AASDK_LOG(warning) << "[Wireless] could not publish the Bluetooth service to "
                            "refuse on: " << error;
      return;
    }
  }
  running_ = false;
  {
    std::lock_guard<std::mutex> lock(handshake_mutex_);
    if (handshake_) {
      handshake_->Stop();
      handshake_.reset();
    }
  }
  // The port goes, because nothing is going to answer on it. The Bluetooth service
  // stays, because that is the whole point.
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  if (fake_phone_) {
    fake_phone_->close(ignored);
    fake_phone_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    summary_.phone_linked = false;
  }
  AASDK_LOG(info) << "[Wireless] no longer offering wireless, refusing phones that ask";
}

void WirelessConnector::Stop() {
  running_ = false;
  declining_ = false;
  nudge_timer_.cancel();
  {
    std::lock_guard<std::mutex> lock(handshake_mutex_);
    if (handshake_) {
      handshake_->Stop();
      handshake_.reset();
    }
  }
  bluetooth_.Stop();
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  if (fake_phone_) {
    fake_phone_->close(ignored);
    fake_phone_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    summary_.bluetooth_ready = false;
    summary_.phone_linked = false;
  }
}

void WirelessConnector::ClearHandlers() {
  on_transport_ = nullptr;
  on_state_ = nullptr;
}

void WirelessConnector::Report(int state, const std::string& message) {
  if (on_state_) {
    on_state_(state, message);
  }
}

WirelessSummary WirelessConnector::summary() const {
  WirelessSummary summary;
  {
    std::lock_guard<std::mutex> lock(summary_mutex_);
    summary = summary_;
  }
  // Asked of the handshake rather than remembered, because a phone that opened the
  // Bluetooth channel and then hung up is not linked, and a status line that said it
  // was would point at the wrong half of the problem.
  std::lock_guard<std::mutex> lock(handshake_mutex_);
  summary.phone_linked = handshake_ != nullptr && handshake_->live();
  return summary;
}

WirelessSettings WirelessConnector::settings() const {
  std::lock_guard<std::mutex> lock(summary_mutex_);
  return settings_;
}

}  // namespace aa
