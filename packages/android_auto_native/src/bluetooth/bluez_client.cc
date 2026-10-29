// SPDX-License-Identifier: GPL-3.0-or-later
#include "bluez_client.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <chrono>

#include <aasdk/Common/Log.hpp>

namespace aa {

const char kAndroidAutoWirelessUuid[] = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

namespace {

constexpr char kObjectPath[] = "/com/flutterheadunit/androidauto/wireless";
constexpr int kCallTimeoutMs = 3000;

// org.bluez.Profile1 as BlueZ expects to find it. Only NewConnection carries anything;
// the other two exist because a profile that does not implement them is one BlueZ
// complains about when the link goes away.
constexpr char kProfileXml[] =
    "<node>"
    "  <interface name='org.bluez.Profile1'>"
    "    <method name='Release'/>"
    "    <method name='NewConnection'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='h' name='fd' direction='in'/>"
    "      <arg type='a{sv}' name='fd_properties' direction='in'/>"
    "    </method>"
    "    <method name='RequestDisconnection'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// A private connection rather than g_bus_get_sync's process wide singleton.
//
// GDBus dispatches a connection's incoming calls on whichever GMainContext was the
// thread default when the connection was created. The shared bus is created by whoever
// asks first, which in a Flutter app could be any library at all, so taking it would
// mean BlueZ's NewConnection arriving on a context this code does not own and may not
// be pumping. A private connection costs one socket and puts the dispatch where it was
// asked for.
GDBusConnection* OpenPrivateSystemBus(std::string* error) {
  GError* failure = nullptr;
  gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, nullptr, &failure);
  if (address == nullptr) {
    *error = std::string("No system D-Bus: ") +
             (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    return nullptr;
  }
  GDBusConnection* bus = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &failure);
  g_free(address);
  if (bus == nullptr) {
    *error = std::string("Could not open the system bus: ") +
             (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    return nullptr;
  }
  return bus;
}

// "/org/bluez/hci0/dev_94_45_60_3C_DD_31" to "94:45:60:3C:DD:31".
std::string AddressFromObjectPath(const std::string& path) {
  const size_t at = path.rfind("/dev_");
  if (at == std::string::npos) {
    return {};
  }
  std::string address = path.substr(at + 5);
  for (char& character : address) {
    if (character == '_') {
      character = ':';
    }
  }
  return address;
}

// Bit 8 to 12 of the Bluetooth class of device is the major class, and 2 is Phone.
bool LooksLikeAPhone(uint32_t device_class) {
  return ((device_class >> 8) & 0x1f) == 0x02;
}

}  // namespace

// The bridge GDBus actually holds a pointer to. A struct rather than a lambda because
// GDBusInterfaceVTable wants plain function pointers and the signature names GLib types
// the header deliberately does not.
struct BluezProfileGlue {
  static void MethodCall(GDBusConnection* connection, const gchar* sender,
                         const gchar* object_path, const gchar* interface_name,
                         const gchar* method_name, GVariant* parameters,
                         GDBusMethodInvocation* invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    auto* client = static_cast<BluezClient*>(user_data);

    if (g_strcmp0(method_name, "NewConnection") == 0) {
      const gchar* device = nullptr;
      gint32 index = 0;
      GVariant* properties = nullptr;
      g_variant_get(parameters, "(oh@a{sv})", &device, &index, &properties);
      if (properties != nullptr) {
        g_variant_unref(properties);
      }

      int fd = -1;
      GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
      GUnixFDList* fds = g_dbus_message_get_unix_fd_list(message);
      if (fds != nullptr) {
        GError* failure = nullptr;
        // Dups, so the descriptor is ours to close.
        fd = g_unix_fd_list_get(fds, index, &failure);
        g_clear_error(&failure);
      }
      // Answer before doing anything with the socket. BlueZ waits for this reply
      // before it considers the profile connected, and the phone waits on BlueZ.
      g_dbus_method_invocation_return_value(invocation, nullptr);
      client->OnNewConnection(fd, device != nullptr ? device : "");
      return;
    }

    if (g_strcmp0(method_name, "Release") == 0) {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      client->OnRelease();
      return;
    }

    // RequestDisconnection. The link is already going; there is nothing to do here
    // that closing the descriptor does not already do at the other end.
    g_dbus_method_invocation_return_value(invocation, nullptr);
  }
};

namespace {

const GDBusInterfaceVTable kProfileVTable = {BluezProfileGlue::MethodCall, nullptr,
                                             nullptr, {nullptr, nullptr, nullptr,
                                                       nullptr}};

}  // namespace

BluezClient::BluezClient() = default;

BluezClient::~BluezClient() { Stop(); }

std::vector<BluetoothDevice> BluezClient::PairedDevices(std::string* error) {
  std::string ignored;
  std::string& failure_text = error != nullptr ? *error : ignored;
  failure_text.clear();

  std::vector<BluetoothDevice> devices;
  GDBusConnection* bus = OpenPrivateSystemBus(&failure_text);
  if (bus == nullptr) {
    return devices;
  }

  GError* failure = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
      nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs,
      nullptr, &failure);
  if (reply == nullptr) {
    failure_text = std::string("Could not ask BlueZ for its devices: ") +
                   (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    g_object_unref(bus);
    return devices;
  }

  GVariantIter* objects = nullptr;
  g_variant_get(reply, "(a{oa{sa{sv}}})", &objects);
  const gchar* path = nullptr;
  GVariantIter* interfaces = nullptr;
  while (g_variant_iter_next(objects, "{&oa{sa{sv}}}", &path, &interfaces)) {
    const gchar* interface_name = nullptr;
    GVariantIter* properties = nullptr;
    while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface_name, &properties)) {
      if (g_strcmp0(interface_name, "org.bluez.Device1") != 0) {
        g_variant_iter_free(properties);
        continue;
      }
      BluetoothDevice device;
      const gchar* key = nullptr;
      GVariant* value = nullptr;
      while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
        if (g_strcmp0(key, "Address") == 0) {
          device.address = g_variant_get_string(value, nullptr);
        } else if (g_strcmp0(key, "Alias") == 0 && device.name.empty()) {
          device.name = g_variant_get_string(value, nullptr);
        } else if (g_strcmp0(key, "Name") == 0) {
          device.name = g_variant_get_string(value, nullptr);
        } else if (g_strcmp0(key, "Paired") == 0) {
          device.paired = g_variant_get_boolean(value) != FALSE;
        } else if (g_strcmp0(key, "Connected") == 0) {
          device.connected = g_variant_get_boolean(value) != FALSE;
        } else if (g_strcmp0(key, "Class") == 0) {
          device.is_phone = LooksLikeAPhone(g_variant_get_uint32(value));
        }
        g_variant_unref(value);
      }
      g_variant_iter_free(properties);
      if (device.paired && !device.address.empty()) {
        devices.push_back(std::move(device));
      }
    }
    g_variant_iter_free(interfaces);
  }
  g_variant_iter_free(objects);
  g_variant_unref(reply);
  g_object_unref(bus);
  return devices;
}

std::string BluezClient::Start(const ProfileOptions& options,
                               ConnectionHandler on_connection, ErrorHandler on_error) {
  if (started_.load()) {
    return {};
  }
  on_connection_ = std::move(on_connection);
  on_error_ = std::move(on_error);
  uuid_ = options.uuid;
  channel_ = options.channel;

  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    ready_ = false;
    ready_error_.clear();
  }
  thread_ = std::thread([this, options]() { Run(options); });

  // Wait for the thread to have published the profile or given up, so the caller gets
  // a straight answer the way UsbConnector::Start does rather than having to watch for
  // an event that may never come.
  std::unique_lock<std::mutex> lock(ready_mutex_);
  if (!ready_cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_; })) {
    lock.unlock();
    Stop();
    return "BlueZ did not answer within five seconds.";
  }
  const std::string error = ready_error_;
  lock.unlock();
  if (!error.empty()) {
    Stop();
    return error;
  }
  started_ = true;
  return {};
}

void BluezClient::Run(ProfileOptions options) {
  context_ = g_main_context_new();
  g_main_context_push_thread_default(context_);
  GMainLoop* loop = g_main_loop_new(context_, FALSE);
  loop_ = loop;

  const std::string error = Publish(options);
  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    ready_ = true;
    ready_error_ = error;
  }
  ready_cv_.notify_all();

  // Checked rather than assumed: a Stop that arrived while Publish was waiting on
  // BlueZ has already quit a loop that was not running yet, and entering it now would
  // never come back out.
  if (error.empty() && !quit_requested_.load()) {
    g_main_loop_run(loop);
  }

  Withdraw();
  loop_ = nullptr;
  g_main_loop_unref(loop);
  g_main_context_pop_thread_default(context_);
  g_main_context_unref(context_);
  context_ = nullptr;
}

std::string BluezClient::Publish(const ProfileOptions& options) {
  std::string error;
  bus_ = OpenPrivateSystemBus(&error);
  if (bus_ == nullptr) {
    return error;
  }

  GError* failure = nullptr;
  GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kProfileXml, &failure);
  if (node == nullptr) {
    error = std::string("Bad profile introspection XML: ") +
            (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    return error;
  }

  object_path_ = kObjectPath;
  object_id_ = g_dbus_connection_register_object(bus_, object_path_.c_str(),
                                                 node->interfaces[0], &kProfileVTable,
                                                 this, nullptr, &failure);
  g_dbus_node_info_unref(node);
  if (object_id_ == 0) {
    error = std::string("Could not export the Bluetooth profile object: ") +
            (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    return error;
  }

  // An RFCOMM channel that is already taken makes BlueZ refuse the registration, and
  // which ones are free depends on what else this machine offers: the audio profiles
  // the host's own software registers take some, and that set is not ours to predict.
  // So the configured channel is a first choice rather than a requirement.
  constexpr int kChannelAttempts = 8;
  for (int attempt = 0; attempt < kChannelAttempts; ++attempt) {
    const int channel = options.channel + attempt;
    if (channel > 30) {
      break;
    }
    error = RegisterWithChannel(options, channel);
    if (error.empty()) {
      channel_ = channel;
      AASDK_LOG(info) << "[Bluetooth] " << options.uuid << " published on RFCOMM "
                      << "channel " << channel
                      << ", waiting for the phone to open it";
      return {};
    }
  }
  return error;
}

std::string BluezClient::RegisterWithChannel(const ProfileOptions& options,
                                             int channel) {
  GError* failure = nullptr;
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "Name",
                        g_variant_new_string(options.name.c_str()));
  // Server, because the phone is the side that opens the channel. It never advertises
  // this UUID itself, so there is nothing here to connect to.
  g_variant_builder_add(&builder, "{sv}", "Role", g_variant_new_string("server"));
  g_variant_builder_add(&builder, "{sv}", "Channel",
                        g_variant_new_uint16(static_cast<guint16>(channel)));
  // The phone will not answer an authorisation prompt that nobody in the car can see.
  g_variant_builder_add(&builder, "{sv}", "RequireAuthorization",
                        g_variant_new_boolean(FALSE));
  // Deliberately no AutoConnect. It would have BlueZ reach out to the phone on this
  // profile whenever it connects, and the phone is the side that decides when to
  // project. Setting it changes the behaviour of a link the host machine's own audio
  // pairing depends on, for no gain.
  g_variant_builder_add(&builder, "{sv}", "Service",
                        g_variant_new_string(options.uuid.c_str()));

  GVariant* reply = g_dbus_connection_call_sync(
      bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile",
      g_variant_new("(osa{sv})", object_path_.c_str(), options.uuid.c_str(), &builder),
      nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &failure);
  if (reply == nullptr) {
    std::string error = std::string("BlueZ refused the Android Auto profile on "
                                    "RFCOMM channel ") +
                        std::to_string(channel) + ": " +
                        (failure != nullptr ? failure->message : "unknown");
    g_clear_error(&failure);
    return error;
  }
  g_variant_unref(reply);
  return {};
}

void BluezClient::Withdraw() {
  if (bus_ == nullptr) {
    return;
  }
  if (!object_path_.empty()) {
    GError* failure = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1",
        "UnregisterProfile", g_variant_new("(o)", object_path_.c_str()), nullptr,
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &failure);
    if (reply != nullptr) {
      g_variant_unref(reply);
    }
    g_clear_error(&failure);
  }
  if (object_id_ != 0) {
    g_dbus_connection_unregister_object(bus_, object_id_);
    object_id_ = 0;
  }
  g_object_unref(bus_);
  bus_ = nullptr;
}

void BluezClient::Stop() {
  quit_requested_ = true;
  if (GMainLoop* loop = loop_.load()) {
    // g_main_loop_quit is one of the few GLib calls that is safe from another thread.
    g_main_loop_quit(loop);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  quit_requested_ = false;
  started_ = false;
  on_connection_ = nullptr;
  on_error_ = nullptr;
}

namespace {

// "94:45:60:3C:DD:31" to "/org/bluez/hci0/dev_94_45_60_3C_DD_31".
std::string DevicePath(const std::string& address) {
  std::string path = "/org/bluez/hci0/dev_" + address;
  for (char& character : path) {
    if (character == ':') {
      character = '_';
    }
  }
  return path;
}

}  // namespace

std::string BluezClient::ConnectedPhone() const {
  std::string ignored;
  for (const BluetoothDevice& device : PairedDevices(&ignored)) {
    if (device.connected && device.is_phone) {
      return device.address;
    }
  }
  // Fall back to any connected paired device: a phone whose class this machine does
  // not recognise is still the phone, and there is rarely more than one.
  for (const BluetoothDevice& device : PairedDevices(&ignored)) {
    if (device.connected) {
      return device.address;
    }
  }
  return {};
}

void BluezClient::Reconnect(const std::string& address) {
  if (bus_ == nullptr || address.empty() || context_ == nullptr) {
    return;
  }
  // Run it on this object's own thread, where the bus connection belongs, and let it
  // block there: the disconnect has to have taken effect before the connect, and
  // nothing else needs that thread meanwhile.
  auto* work = new std::pair<BluezClient*, std::string>(this, address);
  g_main_context_invoke_full(
      context_, G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        auto* job = static_cast<std::pair<BluezClient*, std::string>*>(data);
        BluezClient* client = job->first;
        const std::string path = DevicePath(job->second);

        auto call = [&](const char* method) {
          GError* failure = nullptr;
          GVariant* reply = g_dbus_connection_call_sync(
              client->bus_, "org.bluez", path.c_str(), "org.bluez.Device1", method,
              nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 8000, nullptr, &failure);
          if (reply != nullptr) {
            g_variant_unref(reply);
          } else if (failure != nullptr) {
            AASDK_LOG(warning) << "[Bluetooth] " << method << " on " << path
                               << " failed: " << failure->message;
          }
          g_clear_error(&failure);
        };

        AASDK_LOG(info) << "[Bluetooth] dropping and remaking the link to "
                        << job->second << " so it looks at our services again";
        call("Disconnect");
        // Long enough for the phone to notice the link went. Shorter than this and
        // the reconnect lands before it has torn its own side down, and it does not
        // re-read anything.
        g_usleep(2 * G_USEC_PER_SEC);
        call("Connect");
        return G_SOURCE_REMOVE;
      },
      work,
      [](gpointer data) {
        delete static_cast<std::pair<BluezClient*, std::string>*>(data);
      });
}

void BluezClient::OnNewConnection(int fd, const std::string& device_path) {
  const std::string address = AddressFromObjectPath(device_path);
  AASDK_LOG(info) << "[Bluetooth] accepted a channel from " << address;
  if (fd < 0) {
    if (on_error_) {
      on_error_("BlueZ accepted the Bluetooth channel but handed over no socket.");
    }
    return;
  }
  if (!on_connection_) {
    close(fd);
    return;
  }
  on_connection_(fd, address);
}

void BluezClient::OnRelease() {
  AASDK_LOG(info) << "[Wireless] BlueZ released the Android Auto profile";
}

}  // namespace aa
