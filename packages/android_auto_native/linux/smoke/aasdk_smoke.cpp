// Proves the vendored, ported aasdk actually links and runs on this machine.
//
// Deliberately hardware free so it can run in CI and on a dev box with no phone
// attached. It touches three things that each break in a different way if the Boost
// port regressed:
//
//   1. a symbol from libaasdk, which fails at link time if the library is broken
//   2. an aasdk::Strand, which is the whole point of the io_context port
//   3. a protobuf message from libaap_protobuf, which fails if codegen went wrong

#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>

#include <boost/asio.hpp>
#include <boost/version.hpp>

#include <cstdio>
#include <string>

int main() {
  std::printf("boost      : %d.%d.%d\n", BOOST_VERSION / 100000,
              BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);

  // 1. A real out of line aasdk symbol.
  const std::string channel =
      aasdk::messenger::channelIdToString(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO);
  std::printf("channel    : %s\n", channel.c_str());

  // 2. The ported strand. Post through it and make sure the handler runs on the
  //    io_context, which is what every aasdk channel depends on.
  boost::asio::io_context ioContext;
  aasdk::Strand strand(ioContext);

  bool dispatched = false;
  bool posted = false;
  strand.dispatch([&dispatched] { dispatched = true; });
  strand.post([&posted] { posted = true; });
  ioContext.run();

  if (!dispatched || !posted) {
    std::printf("FAIL: strand did not run its handlers\n");
    return 1;
  }
  if (&strand.get_io_service() != &ioContext) {
    std::printf("FAIL: strand lost track of its io_context\n");
    return 1;
  }
  std::printf("strand     : dispatch, post and get_io_service all behave\n");

  // 3. A generated protobuf message.
  aap_protobuf::service::control::message::ServiceDiscoveryRequest request;
  request.set_device_name("smoke");
  if (request.device_name() != "smoke") {
    std::printf("FAIL: protobuf round trip\n");
    return 1;
  }
  std::printf("protobuf   : ServiceDiscoveryRequest round trips\n");

  std::printf("OK\n");
  return 0;
}
