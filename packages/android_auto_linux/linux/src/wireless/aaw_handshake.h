// The conversation that happens over Bluetooth before wireless projection can start.
//
// Its whole job is to tell the phone two things: which Wi-Fi network to be on, and
// which address and port to connect back to. Once the phone has both, it joins the
// network, opens a TCP connection, and from there the protocol is exactly the one the
// USB path already speaks. Nothing of Android Auto proper happens here.
//
// The messages are the `aap_protobuf::aaw` set, which is not an aasdk channel and has
// no messenger, no SSL and no channel ids. It is its own tiny framing on a raw RFCOMM
// socket: a four byte header of two big endian sixteen bit fields, the payload length
// and the message id, then that many bytes of protobuf.
//
// This file is the only one that names those messages.

#ifndef ANDROID_AUTO_LINUX_WIRELESS_AAW_HANDSHAKE_H_
#define ANDROID_AUTO_LINUX_WIRELESS_AAW_HANDSHAKE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

namespace aa {

// What the head unit offers the phone. Every field is decided before the phone is
// there, so this is a value rather than something the handshake goes and looks up.
struct WirelessOffer {
  std::string ssid;
  std::string passphrase;
  std::string bssid;
  // Where the phone connects back to, once it is on the network.
  std::string ip;
  uint16_t port = 5288;
  // A WifiSecurityMode. 5 is WPA2 personal, which is what a phone expects and what
  // every access point worth using is set to.
  int32_t security = 5;
  // An AccessPointType. 1 is dynamic, meaning the head unit brought this network up
  // for the phone; 0 is static, meaning it is a network that was already there.
  int32_t access_point = 1;
};

// Tells a phone on the other end of `fd` that wireless projection is not on offer,
// and closes the socket. Blocking, one small write, safe to call from any thread.
//
// This exists because saying nothing is worse than saying no. A phone that has been
// introduced to this machine as a wireless car asks for the Android Auto service over
// SDP every five seconds for as long as it is connected over Bluetooth, and an empty
// answer does not discourage it in the slightest: it keeps asking, and keeps showing
// the driver a notification saying it is connecting. Measured on a Pixel 8 Pro: a
// query every 5.1 seconds indefinitely with nothing advertised, against two attempts
// and then eighty seconds of silence when the service exists and refuses.
//
// So a head unit that supports wireless but is not currently offering it should
// publish the service and decline, rather than withdraw the service and say nothing.
void DeclineWirelessOnSocket(int fd);

class AawHandshake : public std::enable_shared_from_this<AawHandshake> {
 public:
  using Pointer = std::shared_ptr<AawHandshake>;
  // Progress, for the event bus. Not a lifecycle state: the connection state the host
  // app sees is driven by the projection session, and this is the running commentary
  // that explains a wireless start that is taking its time.
  using StatusHandler = std::function<void(std::string message)>;
  // The phone said it could not do what was asked. Carries the reason, already turned
  // into something a person can read.
  using FailureHandler = std::function<void(std::string message)>;

  // `fd` is an RFCOMM socket from BlueZ and ownership passes here.
  static Pointer Create(boost::asio::io_context& io_context, int fd, WirelessOffer offer,
                        StatusHandler on_status, FailureHandler on_failure);

  AawHandshake(boost::asio::io_context& io_context, int fd, WirelessOffer offer,
               StatusHandler on_status, FailureHandler on_failure);
  ~AawHandshake();

  AawHandshake(const AawHandshake&) = delete;
  AawHandshake& operator=(const AawHandshake&) = delete;

  // Sends the opening message and starts reading. Safe to call once.
  void Start();

  // Closes the link. Safe from any thread and safe to call twice.
  void Stop();

  // Whether the phone has said it accepted the Wi-Fi details. Not the same question as
  // whether it has connected: it answers, then joins the network, then dials in, and
  // the three are seconds apart.
  bool accepted() const { return accepted_.load(); }

  // Told when a phone has dialled in over Wi-Fi, and when that connection has gone.
  //
  // The start request is what makes a phone on the network open a TCP connection, and
  // sending it to a phone that already has one open makes it obey: it tears the
  // working session down and builds another, forever. So this has to know.
  void NotifyProjecting();
  void NotifyDisconnected();

  // Whether the Bluetooth channel is still open. Goes false when the phone hangs up
  // as well as when Stop is called, which is what makes "phone linked" in the status
  // an answer about now rather than about ever.
  bool live() const { return !stopped_.load(); }

 private:
  void ReadHeader();
  void ReadPayload(uint16_t id, uint16_t length);
  void Handle(uint16_t id, const std::vector<uint8_t>& payload);
  void Send(uint16_t id, const std::string& payload);
  void Flush();
  // Sends the Wi-Fi details whether or not the phone asked for them, see the note in
  // the .cc file about which side is supposed to ask first.
  void OfferNetworkUnprompted();

  // Everything this object touches happens here. The socket is read from an io thread
  // while Stop() arrives from Flutter's platform thread, which is the same shape of
  // race every channel in session/ has and gets the same answer.
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::posix::stream_descriptor socket_;
  boost::asio::steady_timer unprompted_timer_;

  WirelessOffer offer_;
  StatusHandler on_status_;
  FailureHandler on_failure_;

  std::array<uint8_t, 4> header_{};
  std::vector<uint8_t> payload_;
  std::deque<std::string> outbox_;
  bool sending_ = false;
  bool offered_ = false;

  std::atomic<bool> accepted_{false};
  std::atomic<bool> stopped_{false};
  // Whether a phone is connected over Wi-Fi right now. Written from an io thread when
  // a connection is accepted and read on the handshake's own strand.
  std::atomic<bool> projecting_{false};
  // Whether the start request has already been repeated on this channel. One nudge is
  // a nudge; a second is a stutter.
  bool start_resent_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_WIRELESS_AAW_HANDSHAKE_H_
