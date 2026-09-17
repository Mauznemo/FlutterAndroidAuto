#include "aaw_handshake.h"

#include <unistd.h>

#include <chrono>

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/aaw/MessageId.pb.h>
#include <aap_protobuf/aaw/Status.pb.h>
#include <aap_protobuf/aaw/WifiConnectionStatus.pb.h>
#include <aap_protobuf/aaw/WifiInfoRequest.pb.h>
#include <aap_protobuf/aaw/WifiInfoResponse.pb.h>
#include <aap_protobuf/aaw/WifiStartRequest.pb.h>
#include <aap_protobuf/aaw/WifiStartResponse.pb.h>
#include <aap_protobuf/aaw/WifiVersionRequest.pb.h>
#include <aap_protobuf/aaw/WifiVersionResponse.pb.h>

namespace aa {
namespace {

namespace aaw = aap_protobuf::aaw;
namespace wifi_pb = aap_protobuf::service::wifiprojection::message;

// The head unit sends its details and then waits. Which side is supposed to speak
// first is the one piece of this exchange that is not written down anywhere, and the
// implementations that do work disagree about it, so this does both: the start request
// goes out at once, and if the phone has said nothing at all after this long the Wi-Fi
// details follow unprompted rather than the two sides waiting on each other forever.
//
// Sending an answer the phone did not ask for is cheap. A phone that was going to ask
// gets the same message twice, which is idempotent, and a phone that was waiting to be
// told gets told.
constexpr int kUnpromptedOfferMs = 1500;

// Two big endian sixteen bit fields: the payload length, then the message id.
constexpr size_t kHeaderSize = 4;

uint16_t ReadBigEndian16(const uint8_t* bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

void WriteBigEndian16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>(value & 0xff));
}

const char* StatusText(int32_t status) {
  switch (status) {
    case aaw::STATUS_SUCCESS:
      return "accepted";
    case aaw::STATUS_UNSOLICITED_MESSAGE:
      return "the phone did not expect that message";
    case aaw::STATUS_NO_COMPATIBLE_VERSION:
      return "no protocol version in common";
    case aaw::STATUS_WIFI_INACCESSIBLE_CHANNEL:
      return "the phone's radio cannot use that Wi-Fi channel";
    case aaw::STATUS_WIFI_INCORRECT_CREDENTIALS:
      return "the Wi-Fi passphrase was rejected";
    case aaw::STATUS_PROJECTION_ALREADY_STARTED:
      return "the phone is already projecting, over the cable most likely";
    case aaw::STATUS_WIFI_DISABLED:
      return "Wi-Fi is switched off on the phone";
    case aaw::STATUS_WIFI_NOT_YET_STARTED:
      return "the phone's Wi-Fi has not finished starting";
    case aaw::STATUS_INVALID_HOST:
      return "the phone would not accept that address";
    case aaw::STATUS_NO_SUPPORTED_WIFI_CHANNELS:
      return "no Wi-Fi channel in common";
    case aaw::STATUS_INSTRUCT_USER_TO_CHECK_THE_PHONE:
      return "the phone is asking the driver something, look at its screen";
    case aaw::STATUS_PHONE_WIFI_DISABLED:
      return "Wi-Fi is switched off on the phone";
    case aaw::STATUS_WIFI_NETWORK_UNAVAILABLE:
      return "the phone cannot see that network";
    default:
      return "an unknown reason";
  }
}

// What to do about it, for the statuses where the phone's own wording points the
// wrong way. This head unit is often the only thing in the room that can say
// anything, because the machine running it has given up its network to host the one
// the phone is being sent to.
const char* StatusHint(int32_t status) {
  switch (status) {
    case aaw::STATUS_WIFI_INCORRECT_CREDENTIALS:
      // Almost never the passphrase. The phone associates with whatever it finds and
      // reports this when the attempt fails for any reason to do with security, and
      // the common cause is an access point that is not the kind the head unit said
      // it was. A phone told WPA2 personal cannot join a WPA3 network.
      return " That usually means the access point is not WPA2 personal after all: a "
             "phone told WPA2 cannot join a WPA3 or mixed mode network and has no "
             "other way to say so. Check it is RSN with CCMP and management frame "
             "protection off.";
    case aaw::STATUS_WIFI_NETWORK_UNAVAILABLE:
      return " The phone cannot see that network at all. Check the access point is up "
             "and on a channel the phone's radio is allowed to use.";
    case aaw::STATUS_PROJECTION_ALREADY_STARTED:
      return " Unplug the USB cable: the phone is already projecting over it.";
    case aaw::STATUS_PHONE_WIFI_DISABLED:
    case aaw::STATUS_WIFI_DISABLED:
      return " Turn the phone's Wi-Fi on. A phone sharing its mobile data over Wi-Fi "
             "counts as having none free.";
    default:
      return "";
  }
}

const char* MessageName(uint16_t id) {
  switch (id) {
    case aaw::WIFI_START_REQUEST:
      return "WifiStartRequest";
    case aaw::WIFI_INFO_REQUEST:
      return "WifiInfoRequest";
    case aaw::WIFI_INFO_RESPONSE:
      return "WifiInfoResponse";
    case aaw::WIFI_VERSION_REQUEST:
      return "WifiVersionRequest";
    case aaw::WIFI_VERSION_RESPONSE:
      return "WifiVersionResponse";
    case aaw::WIFI_CONNECTION_STATUS:
      return "WifiConnectionStatus";
    case aaw::WIFI_START_RESPONSE:
      return "WifiStartResponse";
    default:
      return "an unknown message";
  }
}

}  // namespace

void DeclineWirelessOnSocket(int fd) {
  if (fd < 0) {
    return;
  }

  aaw::WifiConnectionStatus status;
  // The truthful one: this head unit's Wi-Fi is not on offer.
  status.set_status(aaw::STATUS_WIFI_DISABLED);
  const std::string payload = status.SerializeAsString();

  std::string frame;
  WriteBigEndian16(&frame, static_cast<uint16_t>(payload.size()));
  WriteBigEndian16(&frame, aaw::WIFI_CONNECTION_STATUS);
  frame.append(payload);

  // One small write on a socket nobody else has. Blocking is fine and a short write
  // is not worth handling: the phone either hears the refusal or asks again later,
  // which is the state this is trying to improve on rather than to guarantee.
  const ssize_t written = ::write(fd, frame.data(), frame.size());
  (void)written;
  ::close(fd);
  AASDK_LOG(info) << "[Wireless] told the phone that wireless is not on offer";
}

AawHandshake::Pointer AawHandshake::Create(boost::asio::io_context& io_context, int fd,
                                           WirelessOffer offer, StatusHandler on_status,
                                           FailureHandler on_failure) {
  return std::make_shared<AawHandshake>(io_context, fd, std::move(offer),
                                        std::move(on_status), std::move(on_failure));
}

AawHandshake::AawHandshake(boost::asio::io_context& io_context, int fd,
                           WirelessOffer offer, StatusHandler on_status,
                           FailureHandler on_failure)
    : strand_(boost::asio::make_strand(io_context)),
      socket_(io_context, fd),
      unprompted_timer_(io_context),
      offer_(std::move(offer)),
      on_status_(std::move(on_status)),
      on_failure_(std::move(on_failure)) {}

AawHandshake::~AawHandshake() {
  // Deliberately not Stop(): that posts, and posting needs shared_from_this, which is
  // already gone by the time a destructor runs. Nothing else can be touching the
  // socket either, because every handler holds a reference and the last of them going
  // away is what brought us here, so asio's own destructor closing the descriptor is
  // both correct and enough.
  stopped_ = true;
}

void AawHandshake::Start() {
  auto self = shared_from_this();
  boost::asio::post(strand_, [this, self]() {
    if (stopped_.load()) {
      return;
    }
    aaw::WifiStartRequest request;
    request.set_ip_address(offer_.ip);
    request.set_port(offer_.port);
    Send(aaw::WIFI_START_REQUEST, request.SerializeAsString());
    AASDK_LOG(info) << "[Wireless] told the phone to connect to " << offer_.ip << ":"
                    << offer_.port;
    if (on_status_) {
      on_status_("Bluetooth channel open. Offering " + offer_.ssid + " and " +
                 offer_.ip + ":" + std::to_string(offer_.port) + ".");
    }

    unprompted_timer_.expires_after(std::chrono::milliseconds(kUnpromptedOfferMs));
    unprompted_timer_.async_wait(boost::asio::bind_executor(
        strand_, [this, self](const boost::system::error_code& ec) {
          if (!ec) {
            OfferNetworkUnprompted();
          }
        }));

    ReadHeader();
  });
}

void AawHandshake::OfferNetworkUnprompted() {
  if (offered_ || stopped_.load()) {
    return;
  }
  AASDK_LOG(debug) << "[Wireless] the phone asked for nothing, offering the network "
                      "anyway";
  aaw::WifiInfoResponse response;
  response.set_ssid(offer_.ssid);
  response.set_password(offer_.passphrase);
  response.set_bssid(offer_.bssid);
  response.set_security_mode(static_cast<wifi_pb::WifiSecurityMode>(offer_.security));
  response.set_access_point_type(
      static_cast<wifi_pb::AccessPointType>(offer_.access_point));
  Send(aaw::WIFI_INFO_RESPONSE, response.SerializeAsString());
  offered_ = true;
}

void AawHandshake::ReadHeader() {
  if (stopped_.load()) {
    return;
  }
  auto self = shared_from_this();
  boost::asio::async_read(
      socket_, boost::asio::buffer(header_),
      boost::asio::bind_executor(
          strand_, [this, self](const boost::system::error_code& ec, size_t) {
            if (ec) {
              if (!stopped_.exchange(true)) {
                AASDK_LOG(info) << "[Wireless] Bluetooth channel closed: "
                                << ec.message();
              }
              return;
            }
            const uint16_t length = ReadBigEndian16(header_.data());
            const uint16_t id = ReadBigEndian16(header_.data() + 2);
            ReadPayload(id, length);
          }));
}

void AawHandshake::ReadPayload(uint16_t id, uint16_t length) {
  if (length == 0) {
    Handle(id, {});
    ReadHeader();
    return;
  }
  payload_.assign(length, 0);
  auto self = shared_from_this();
  boost::asio::async_read(
      socket_, boost::asio::buffer(payload_),
      boost::asio::bind_executor(
          strand_, [this, self, id](const boost::system::error_code& ec, size_t) {
            if (ec) {
              if (!stopped_.exchange(true)) {
                AASDK_LOG(info) << "[Wireless] Bluetooth channel closed mid message: "
                                << ec.message();
              }
              return;
            }
            Handle(id, payload_);
            ReadHeader();
          }));
}

void AawHandshake::Handle(uint16_t id, const std::vector<uint8_t>& payload) {
  // Info rather than debug, unlike every other per message trace in this project.
  // There are at most a handful of these per connection, and they are the only record
  // of what a phone actually said during a handshake that has never been seen to
  // succeed. Debug is not an option here: it also turns on the video frame log, which
  // buries this at thirty lines a second.
  AASDK_LOG(info) << "[Wireless] " << MessageName(id) << " (" << id << "), "
                  << payload.size() << " bytes";
  const void* bytes = payload.empty() ? nullptr : payload.data();
  const int size = static_cast<int>(payload.size());

  switch (id) {
    case aaw::WIFI_INFO_REQUEST: {
      // Cancel the unprompted offer: the phone did ask after all.
      unprompted_timer_.cancel();
      aaw::WifiInfoResponse response;
      response.set_ssid(offer_.ssid);
      response.set_password(offer_.passphrase);
      response.set_bssid(offer_.bssid);
      response.set_security_mode(
          static_cast<wifi_pb::WifiSecurityMode>(offer_.security));
      response.set_access_point_type(
          static_cast<wifi_pb::AccessPointType>(offer_.access_point));
      Send(aaw::WIFI_INFO_RESPONSE, response.SerializeAsString());
      offered_ = true;
      AASDK_LOG(info) << "[Wireless] offered " << offer_.ssid << " to the phone";
      if (on_status_) {
        on_status_("The phone asked for the network details and has them.");
      }
      return;
    }

    case aaw::WIFI_VERSION_REQUEST: {
      // Four numbers whose meaning is not published and which this head unit has never
      // been asked for. Answering with the shape of the message rather than nothing is
      // the lesser risk: a phone that asks and gets silence stops, and a phone that
      // dislikes the numbers says so in a status message that will end up in the log.
      aaw::WifiVersionResponse response;
      response.set_unknown_value_a(1);
      response.set_unknown_value_b(0);
      response.set_unknown_value_d(0);
      Send(aaw::WIFI_VERSION_RESPONSE, response.SerializeAsString());
      AASDK_LOG(warning) << "[Wireless] the phone asked for a version this head unit "
                            "can only guess at, see aaw_handshake.cc";
      return;
    }

    case aaw::WIFI_CONNECTION_STATUS: {
      aaw::WifiConnectionStatus status;
      if (!status.ParseFromArray(bytes, size)) {
        return;
      }
      const char* text = StatusText(status.status());
      AASDK_LOG(info) << "[Wireless] the phone reports: " << text
                      << (status.has_error_message() ? " (" + status.error_message() + ")"
                                                     : std::string());
      if (status.status() == aaw::STATUS_SUCCESS) {
        accepted_ = true;
        if (projecting_.load() || start_resent_) {
          // The phone says this whenever it feels like reporting its network, which
          // includes while it is happily projecting. Answering with another start
          // request is an instruction to reconnect, and it obeys: sixteen sessions in
          // four minutes, each one dying a second after its first video frame.
          AASDK_LOG(debug) << "[Wireless] the phone reports its network again, "
                              "nothing to do";
          return;
        }
        start_resent_ = true;
        if (on_status_) {
          on_status_("The phone is on the network. Asking it to connect.");
        }
        // And ask again, now that the phone is actually on the network.
        //
        // Read the message names as what they mean: the info exchange hands over
        // credentials, this status is the phone saying it has joined, and the start
        // request is what tells it to dial. The one sent when the channel opened was
        // answered before the phone had a network to dial over, which is an odd
        // moment to be told where to connect to. Sending it again here costs one
        // small message and removes the possibility that the phone is sitting on the
        // network waiting to be asked.
        aaw::WifiStartRequest request;
        request.set_ip_address(offer_.ip);
        request.set_port(offer_.port);
        Send(aaw::WIFI_START_REQUEST, request.SerializeAsString());
        AASDK_LOG(info) << "[Wireless] phone is on the network, asked it again to "
                        << "connect to " << offer_.ip << ":" << offer_.port;
        return;
      }
      if (on_failure_) {
        on_failure_(std::string("The phone would not start wireless Android Auto: ") +
                    text + "." + StatusHint(status.status()));
      }
      return;
    }

    case aaw::WIFI_START_RESPONSE: {
      aaw::WifiStartResponse response;
      if (!response.ParseFromArray(bytes, size)) {
        return;
      }
      const char* text = StatusText(response.status());
      AASDK_LOG(info) << "[Wireless] start response: " << text;
      if (response.status() == aaw::STATUS_SUCCESS) {
        accepted_ = true;
        if (on_status_) {
          on_status_("The phone accepted the connection details.");
        }
        return;
      }
      if (on_failure_) {
        on_failure_(std::string("The phone refused the connection details: ") + text +
                    "." + StatusHint(response.status()));
      }
      return;
    }

    default:
      // Includes the two the head unit sends. A phone echoing one back is not a fault
      // worth reporting, and the debug line above has already recorded it.
      return;
  }
}

void AawHandshake::Send(uint16_t id, const std::string& payload) {
  std::string frame;
  frame.reserve(kHeaderSize + payload.size());
  WriteBigEndian16(&frame, static_cast<uint16_t>(payload.size()));
  WriteBigEndian16(&frame, id);
  frame.append(payload);
  outbox_.push_back(std::move(frame));
  Flush();
}

void AawHandshake::Flush() {
  if (sending_ || outbox_.empty() || stopped_.load()) {
    return;
  }
  sending_ = true;
  auto self = shared_from_this();
  boost::asio::async_write(
      socket_, boost::asio::buffer(outbox_.front()),
      boost::asio::bind_executor(
          strand_, [this, self](const boost::system::error_code& ec, size_t) {
            sending_ = false;
            if (ec) {
              AASDK_LOG(warning) << "[Wireless] could not write to the phone: "
                                 << ec.message();
              return;
            }
            outbox_.pop_front();
            Flush();
          }));
}

void AawHandshake::NotifyProjecting() { projecting_ = true; }

void AawHandshake::NotifyDisconnected() {
  projecting_ = false;
  // Deliberately not clearing start_resent_. The phone is on the network and knows
  // where to dial; a connection that dropped is one it will retry on its own, and a
  // head unit that starts nudging again the moment it notices is how the loop above
  // began.
}

void AawHandshake::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  // Closed on the strand rather than here. Stop() arrives from Flutter's platform
  // thread while a read is outstanding on an io thread, and asio makes no promise
  // about a descriptor closed from under one of its own operations. Hopping costs a
  // few microseconds and makes the teardown ordinary.
  //
  // close() rather than release(): the descriptor came from BlueZ and this object owns
  // it. The phone sees the channel drop, which is what it should see.
  auto self = shared_from_this();
  boost::asio::post(strand_, [this, self]() {
    boost::system::error_code ignored;
    unprompted_timer_.cancel();
    socket_.close(ignored);
  });
}

}  // namespace aa
