// The only file that talks to BlueZ, and the only one that names a D-Bus type.
//
// Wireless Android Auto starts over Bluetooth: the head unit advertises an RFCOMM
// service, the phone opens a channel to it, and the Wi-Fi details are exchanged there
// before any of the projection protocol happens. Getting that channel means SDP, and
// the only supported way to publish an SDP record on this system is BlueZ's D-Bus
// ProfileManager. libbluetooth's own SDP API is deprecated and its development package
// is not even installed here.
//
// GLib is already linked, because this is a GTK Flutter plugin, so GDBus costs nothing
// extra. It does bring one constraint: GDBus dispatches on a GMainContext, and this
// plugin must not assume anything about the embedder's. So this class owns a private
// context on its own thread, and every call into BlueZ happens there. The one thing
// that crosses back out is the connected file descriptor, which is handed to the owner
// and never touched here again: from that point on it is an ordinary socket that the
// io_context can read, with no GLib anywhere near it.
//
// Nothing here changes how the machine presents itself on Bluetooth. Registering a
// profile adds one UUID to the adapter's SDP record and takes nothing away: pairing,
// the adapter class, discoverability, the audio profiles and any agent already running
// are all left exactly as they were. A head unit whose own software pairs the phone for
// A2DP and hands free calling keeps working unchanged, which is the point, because that
// software is what makes the phone treat this machine as a car in the first place.

#ifndef ANDROID_AUTO_LINUX_BLUETOOTH_BLUEZ_CLIENT_H_
#define ANDROID_AUTO_LINUX_BLUETOOTH_BLUEZ_CLIENT_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GMainContext GMainContext;
typedef struct _GMainLoop GMainLoop;

namespace aa {

// The UUID Android looks for to decide a Bluetooth device can project wirelessly.
//
// Publicly known and the same in every open implementation. A phone only learns it
// from the head unit's SDP record, never the other way round: this Pixel advertises no
// such UUID of its own, which is what settles who listens and who connects.
extern const char kAndroidAutoWirelessUuid[];

// One device BlueZ knows about.
struct BluetoothDevice {
  std::string address;
  std::string name;
  bool paired = false;
  bool connected = false;
  // Whether BlueZ thinks this is a phone, from the device class it reports. Only a
  // hint, so the host app can put the likely entry at the top of a picker rather than
  // the user's headphones.
  bool is_phone = false;
};

// What to publish. The defaults are what a head unit wants.
struct ProfileOptions {
  std::string uuid = kAndroidAutoWirelessUuid;
  std::string name = "Android Auto Wireless";
  // An explicit RFCOMM channel, and it has to be explicit.
  //
  // BlueZ builds the SDP record from what RegisterProfile is given, and with no
  // Channel it produces a record whose protocol descriptor list is L2CAP and nothing
  // else: no RFCOMM entry, no channel number. A phone asks for this UUID by name on
  // every Bluetooth connection, reads that back, finds nothing to dial, and gives up
  // without another packet. Neither end logs anything, which is why this looks like a
  // phone ignoring the head unit rather than a malformed record.
  //
  // The number itself means nothing to the phone, which reads it out of the record.
  // It has to be free, so BluezClient tries the ones after it if BlueZ refuses.
  int channel = 8;
};

class BluezClient {
 public:
  // Called with an accepted RFCOMM connection. `fd` is an ordinary socket and
  // ownership passes to the callee, which must close it. Runs on this object's own
  // thread, so the callee has to hop to wherever it actually works.
  using ConnectionHandler = std::function<void(int fd, std::string address)>;
  using ErrorHandler = std::function<void(std::string)>;

  BluezClient();
  ~BluezClient();

  BluezClient(const BluezClient&) = delete;
  BluezClient& operator=(const BluezClient&) = delete;

  // Every paired device, newest information BlueZ has. Blocks for up to a second, so
  // call it from Dart rather than from a hot path. Fills `error` and returns empty
  // when BlueZ cannot be reached, which on a machine with no Bluetooth is not a fault.
  static std::vector<BluetoothDevice> PairedDevices(std::string* error);

  // Publishes the RFCOMM service and starts listening for phones. Returns an error
  // string on failure, empty on success. Idempotent: a second call with the profile
  // already up is a no-op.
  std::string Start(const ProfileOptions& options, ConnectionHandler on_connection,
                    ErrorHandler on_error);

  // Withdraws the service and stops the thread. Safe to call when not started.
  void Stop();

  bool started() const { return started_.load(); }

  // The RFCOMM channel BlueZ allocated, or 0 before the profile is up. Only interesting
  // when something has to be diagnosed with sdptool from outside the app.
  int channel() const { return channel_.load(); }

  // Drops and remakes the Bluetooth link to `address`, which is the only thing that
  // makes a phone look at this machine's services again.
  //
  // Needed because refusing works: a phone that has been told twice that wireless is
  // not on offer stops asking, and then does not notice when it is. It re-reads the
  // service list when the link comes up, so that is what this does.
  //
  // `Device1.ConnectProfile` on the Android Auto UUID is not an alternative. The phone
  // does not advertise that service, only asks for it, so BlueZ answers "No more
  // profiles to connect to" and nothing happens.
  //
  // This costs the phone's Bluetooth audio a few seconds, so it belongs behind a check
  // that the phone has actually gone quiet. Blocks the caller only long enough to
  // queue the work. Best effort: errors are logged and nothing is retried.
  void Reconnect(const std::string& address);

  // The first paired device that is connected right now, or empty. For finding the
  // phone to reconnect when the host app has not named one.
  std::string ConnectedPhone() const;

 private:
  void Run(ProfileOptions options);
  // Everything below runs on thread_, inside context_.
  std::string Publish(const ProfileOptions& options);
  // One attempt at one RFCOMM channel. Returns an error string, empty on success.
  std::string RegisterWithChannel(const ProfileOptions& options, int channel);
  void Withdraw();

  // Called from the exported org.bluez.Profile1 object, by the free function in the
  // .cc file that GDBus actually holds a pointer to.
  void OnNewConnection(int fd, const std::string& device_path);
  void OnRelease();

  friend struct BluezProfileGlue;

  ConnectionHandler on_connection_;
  ErrorHandler on_error_;

  std::thread thread_;
  GMainContext* context_ = nullptr;
  // Written on thread_ and read by Stop from whoever asked, so atomic. Paired with
  // quit_requested_, which covers the window before it exists at all: a Stop that
  // beat the thread to creating the loop would otherwise quit nothing and then block
  // forever joining a thread that is about to start running one.
  std::atomic<GMainLoop*> loop_{nullptr};
  std::atomic<bool> quit_requested_{false};
  GDBusConnection* bus_ = nullptr;
  unsigned int object_id_ = 0;
  std::string object_path_;
  std::string uuid_;
  std::atomic<int> channel_{0};
  std::atomic<bool> started_{false};

  // Start() waits here for the thread to have published the profile or failed, so that
  // it can answer synchronously the way UsbConnector::Start does.
  std::mutex ready_mutex_;
  std::condition_variable ready_cv_;
  bool ready_ = false;
  std::string ready_error_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_BLUETOOTH_BLUEZ_CLIENT_H_
