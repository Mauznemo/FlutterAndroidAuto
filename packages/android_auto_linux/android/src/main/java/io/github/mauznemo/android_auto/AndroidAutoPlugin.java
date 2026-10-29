// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.mauznemo.android_auto;

import android.app.PendingIntent;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.MacAddress;
import android.net.wifi.SoftApConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.NonNull;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.view.TextureRegistry;

/**
 * The Android entry point of the android_auto plugin.
 *
 * <p>Everything the Dart side does goes over FFI into the same native core Linux runs, so
 * this class has no method channel. It exists for the things only Java can reach: loading
 * the library early enough that the core can find this class, Flutter's texture registry,
 * which hands out the {@link Surface} the projected picture is drawn into, and {@link
 * UsbManager}, which is the only way an app gets at a phone on the cable, and Bluetooth and
 * Wi-Fi, which wireless projection needs and which are only reachable from Java.
 *
 * <p>The native side calls the static methods below through JNI and is called back
 * through the {@code native} ones. A {@code handle} is the address of the native present
 * adapter a surface belongs to, which is all either side needs to find the other.
 */
public class AndroidAutoPlugin implements FlutterPlugin {
  private static final String TAG = "AndroidAuto";

  private static final Object lock = new Object();
  private static TextureRegistry textures;
  private static Context context;
  private static final Map<Long, TextureRegistry.SurfaceProducer> producers = new HashMap<>();
  private static final Handler main = new Handler(Looper.getMainLooper());

  // USB, all on the main thread except releaseUsb.
  private static final String ACTION_USB_PERMISSION =
      "io.github.mauznemo.android_auto.USB_PERMISSION";
  // Google's vendor id, and the two product ids a phone takes on in accessory mode.
  private static final int GOOGLE_VENDOR = 0x18d1;
  private static final int AOAP = 0x2d00;
  private static final int AOAP_WITH_ADB = 0x2d01;
  private static boolean usbWatching = false;
  private static BroadcastReceiver usbReceiver;
  // Devices handed to the core and not yet released, by device name, so a device seen
  // both at enumeration and in an attach broadcast is offered once.
  private static final Set<String> usbBusy = new HashSet<>();
  // Devices a permission dialog is showing for, so it is not shown twice.
  private static final Set<String> usbAsking = new HashSet<>();
  // When each device without permission was first seen. See considerUsb.
  private static final Map<String, Long> usbFirstSeen = new HashMap<>();
  // How long a device is left alone before the app asks about it itself.
  private static final long USB_ASK_AFTER_MS = 2000;
  private static final long USB_POLL_MS = 1000;
  // Rescans while watching. The permission can be granted without the app's own request
  // ever being answered: the system's "open with" chooser, which the host app's
  // USB_DEVICE_ATTACHED filter brings up, grants it and starts the activity instead.
  // Seen on Android 13, both for the chooser and for the request it closes alongside.
  private static final Runnable usbPoll =
      new Runnable() {
        @Override
        public void run() {
          if (!usbWatching) {
            return;
          }
          scanUsb();
          main.postDelayed(this, USB_POLL_MS);
        }
      };
  private static final Map<Integer, UsbDeviceConnection> usbConnections = new HashMap<>();
  private static final Map<Integer, String> usbNames = new HashMap<>();

  // Whether the native core is in this APK. It is built for arm64-v8a, armeabi-v7a and
  // x86_64; an app built for anything else still has to start, with this plugin
  // unavailable, rather than die in a static initialiser before Flutter is up. The Dart
  // side then reports the library as missing when a session is started.
  private static final boolean loaded = load();

  private static boolean load() {
    try {
      System.loadLibrary("android_auto_native");
      return true;
    } catch (UnsatisfiedLinkError e) {
      Log.e(TAG, "The android_auto native library is not built for this device", e);
      return false;
    }
  }

  @Override
  public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
    synchronized (lock) {
      textures = binding.getTextureRegistry();
      context = binding.getApplicationContext();
    }
    if (loaded) {
      nativeAttach(binding.getApplicationContext());
    }
  }

  @Override
  public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
    synchronized (lock) {
      for (TextureRegistry.SurfaceProducer producer : producers.values()) {
        producer.release();
      }
      producers.clear();
      textures = null;
      context = null;
    }
  }

  /**
   * Creates the texture a present adapter draws into and returns its id, or -1 when no
   * engine is attached. The surface itself arrives through {@link #nativeSurfaceAvailable}
   * before this returns, and again whenever Flutter replaces it.
   */
  @SuppressWarnings("unused") // Called from native code.
  static long createSurface(long handle, int width, int height) {
    final long[] id = {-1};
    runOnMain(
        () -> {
          final TextureRegistry registry;
          synchronized (lock) {
            registry = textures;
          }
          if (registry == null) {
            return;
          }
          final TextureRegistry.SurfaceProducer producer = registry.createSurfaceProducer();
          producer.setSize(Math.max(1, width), Math.max(1, height));
          producer.setCallback(
              new TextureRegistry.SurfaceProducer.Callback() {
                @Override
                public void onSurfaceAvailable() {
                  nativeSurfaceAvailable(handle, producer.getSurface());
                }

                @Override
                public void onSurfaceCleanup() {
                  // Must not return while the native side is still drawing into it.
                  nativeSurfaceCleanup(handle);
                }
              });
          synchronized (lock) {
            producers.put(handle, producer);
          }
          nativeSurfaceAvailable(handle, producer.getSurface());
          id[0] = producer.id();
        });
    return id[0];
  }

  /**
   * Gives the texture a new size in pixels. Called from the native render thread, so it
   * is posted rather than run: the producer belongs to the main thread, and the render
   * thread must not wait on it.
   */
  @SuppressWarnings("unused") // Called from native code.
  static void resizeSurface(long handle, int width, int height) {
    main.post(
        () -> {
          final TextureRegistry.SurfaceProducer producer;
          synchronized (lock) {
            producer = producers.get(handle);
          }
          if (producer == null) {
            return;
          }
          if (producer.getWidth() == width && producer.getHeight() == height) {
            return;
          }
          producer.setSize(width, height);
          nativeSurfaceAvailable(handle, producer.getSurface());
        });
  }

  /** Releases the texture. */
  @SuppressWarnings("unused") // Called from native code.
  static void releaseSurface(long handle) {
    runOnMain(
        () -> {
          final TextureRegistry.SurfaceProducer producer;
          synchronized (lock) {
            producer = producers.remove(handle);
          }
          if (producer != null) {
            nativeSurfaceCleanup(handle);
            producer.release();
          }
        });
  }

  /**
   * Runs {@code work} on the main thread and waits for it. Dart calls into the core on
   * the platform thread, which is the main thread on every Flutter that merges the two,
   * so this is normally a direct call.
   */
  private static void runOnMain(Runnable work) {
    if (Looper.myLooper() == Looper.getMainLooper()) {
      work.run();
      return;
    }
    final CountDownLatch done = new CountDownLatch(1);
    main.post(
        () -> {
          try {
            work.run();
          } finally {
            done.countDown();
          }
        });
    try {
      if (!done.await(2, TimeUnit.SECONDS)) {
        Log.w(TAG, "The main thread did not run a texture call within two seconds");
      }
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
  }

  /** Starts watching for phones on USB. Called from native code, on the platform thread. */
  @SuppressWarnings("unused") // Called from native code.
  static void startUsb() {
    main.post(
        () -> {
          final Context app;
          synchronized (lock) {
            app = context;
          }
          if (app == null || usbWatching) {
            return;
          }
          usbWatching = true;
          usbReceiver =
              new BroadcastReceiver() {
                @Override
                public void onReceive(Context ignored, Intent intent) {
                  onUsbIntent(intent);
                }
              };
          final IntentFilter filter = new IntentFilter();
          filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
          filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
          filter.addAction(ACTION_USB_PERMISSION);
          // Not exported: the attach broadcasts come from the system, which reaches a
          // receiver that is not, and the permission answer is our own explicit intent.
          if (Build.VERSION.SDK_INT >= 33) {
            app.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
          } else {
            app.registerReceiver(usbReceiver, filter);
          }
          scanUsb();
          main.postDelayed(usbPoll, USB_POLL_MS);
        });
  }

  /** Stops watching. Devices already handed over stay open until released. */
  @SuppressWarnings("unused") // Called from native code.
  static void stopUsb() {
    main.post(
        () -> {
          if (!usbWatching) {
            return;
          }
          usbWatching = false;
          final Context app;
          synchronized (lock) {
            app = context;
          }
          if (app != null && usbReceiver != null) {
            app.unregisterReceiver(usbReceiver);
          }
          usbReceiver = null;
          main.removeCallbacks(usbPoll);
          usbAsking.clear();
          usbFirstSeen.clear();
        });
  }

  /** Offers every attached device again that is not handed over right now. */
  @SuppressWarnings("unused") // Called from native code.
  static void rescanUsb() {
    main.post(
        () -> {
          if (usbWatching) {
            scanUsb();
          }
        });
  }

  /** Closes a device the core has finished with. Called from any native thread. */
  @SuppressWarnings("unused") // Called from native code.
  static void releaseUsb(int fd) {
    main.post(
        () -> {
          final UsbDeviceConnection connection = usbConnections.remove(fd);
          final String name = usbNames.remove(fd);
          if (name != null) {
            usbBusy.remove(name);
          }
          if (connection != null) {
            connection.close();
          }
        });
  }

  private static UsbManager usbManager() {
    synchronized (lock) {
      return context == null ? null : (UsbManager) context.getSystemService(Context.USB_SERVICE);
    }
  }

  private static void scanUsb() {
    final UsbManager manager = usbManager();
    if (manager == null) {
      return;
    }
    for (UsbDevice device : manager.getDeviceList().values()) {
      considerUsb(device);
    }
  }

  private static void onUsbIntent(Intent intent) {
    final UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    if (device == null) {
      return;
    }
    final String action = intent.getAction();
    if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
      considerUsb(device);
    } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
      // The core notices on its own, through failed transfers, and releases the
      // descriptor. Only a dialog that was still waiting is forgotten here.
      usbAsking.remove(device.getDeviceName());
      usbFirstSeen.remove(device.getDeviceName());
    } else if (ACTION_USB_PERMISSION.equals(action)) {
      Log.i(
          TAG,
          "Permission answer for "
              + describe(device)
              + ": "
              + intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false));
      usbAsking.remove(device.getDeviceName());
      if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
        openUsb(device);
      } else {
        Log.i(TAG, "Not allowed to use " + describe(device));
      }
    }
  }

  private static void considerUsb(UsbDevice device) {
    if (!usbWatching || !mightBePhone(device)) {
      return;
    }
    final String name = device.getDeviceName();
    if (usbBusy.contains(name)) {
      return;
    }
    final UsbManager manager = usbManager();
    if (manager == null) {
      return;
    }
    if (manager.hasPermission(device)) {
      usbAsking.remove(name);
      usbFirstSeen.remove(name);
      openUsb(device);
      return;
    }
    if (usbAsking.contains(name)) {
      return;
    }
    // Not at once. A device the user has set to always open this app is granted by the
    // system a moment after it arrives, and a host app with an attach filter shows the
    // system's own chooser; asking straight away stacked a second dialog on top of it.
    final long now = android.os.SystemClock.uptimeMillis();
    final Long seen = usbFirstSeen.get(name);
    if (seen == null) {
      usbFirstSeen.put(name, now);
      return;
    }
    if (now - seen < USB_ASK_AFTER_MS) {
      return;
    }
    final Context app;
    synchronized (lock) {
      app = context;
    }
    if (app == null) {
      return;
    }
    usbAsking.add(name);
    Log.i(TAG, "Asking for permission to use " + describe(device));
    // Explicit and mutable: the system fills in which device and whether it was
    // allowed, and from Android 14 a mutable intent has to name its package.
    final Intent answer = new Intent(ACTION_USB_PERMISSION).setPackage(app.getPackageName());
    final int flags =
        Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
    manager.requestPermission(device, PendingIntent.getBroadcast(app, 0, answer, flags));
  }

  private static void openUsb(UsbDevice device) {
    final UsbManager manager = usbManager();
    if (manager == null || !usbWatching) {
      return;
    }
    final String name = device.getDeviceName();
    if (usbBusy.contains(name)) {
      return;
    }
    final UsbDeviceConnection connection = manager.openDevice(device);
    if (connection == null) {
      Log.w(TAG, "Could not open " + describe(device));
      return;
    }
    final int fd = connection.getFileDescriptor();
    Log.i(TAG, "Handing " + describe(device) + " to the head unit");
    usbBusy.add(name);
    usbConnections.put(fd, connection);
    usbNames.put(fd, name);
    nativeUsbDevice(fd, isAccessory(device), describe(device));
  }

  private static boolean isAccessory(UsbDevice device) {
    return device.getVendorId() == GOOGLE_VENDOR
        && (device.getProductId() == AOAP || device.getProductId() == AOAP_WITH_ADB);
  }

  /**
   * Whether a device is worth asking about. Every phone shows at least one vendor
   * specific or still image interface (adb, MTP, PTP), or none at all when it is set to
   * charge only. A hub, a keyboard or a flash drive shows neither, and asking the user
   * for permission to use those would only be noise.
   */
  private static boolean mightBePhone(UsbDevice device) {
    if (isAccessory(device)) {
      return true;
    }
    if (device.getDeviceClass() == UsbConstants.USB_CLASS_HUB) {
      return false;
    }
    final int count = device.getInterfaceCount();
    if (count == 0) {
      return true;
    }
    for (int i = 0; i < count; i++) {
      final UsbInterface usbInterface = device.getInterface(i);
      final int interfaceClass = usbInterface.getInterfaceClass();
      if (interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC
          || interfaceClass == UsbConstants.USB_CLASS_STILL_IMAGE) {
        return true;
      }
    }
    return false;
  }

  private static String describe(UsbDevice device) {
    String product = null;
    try {
      product = device.getProductName();
    } catch (SecurityException ignored) {
      // Readable only with permission on some versions.
    }
    return String.format(
        "%s %04x:%04x (%s)",
        product == null ? "USB device" : product,
        device.getVendorId(),
        device.getProductId(),
        device.getDeviceName());
  }

  // Bluetooth. The server socket and the phones connected to it.
  private static BluetoothServerSocket rfcommServer;
  private static final Set<BluetoothSocket> rfcommSockets = new HashSet<>();
  // The phone's headset gateway, which a head unit with no hands free profile of its own
  // connects to so that the phone treats it as something to project to. See nudgePhone.
  private static final UUID HEADSET_GATEWAY = UUID.fromString("00001112-0000-1000-8000-00805f9b34fb");
  private static BluetoothSocket headsetLink;
  // Phones with a Bluetooth link up, most recent last, from ACL broadcasts, because
  // Android has no public way to ask whether a bonded device is connected.
  private static final java.util.LinkedHashSet<String> linkedPhones = new java.util.LinkedHashSet<>();
  private static BroadcastReceiver aclReceiver;

  private static BluetoothAdapter bluetoothAdapter() {
    synchronized (lock) {
      if (context == null) {
        return null;
      }
      final BluetoothManager manager =
          (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
      return manager == null ? null : manager.getAdapter();
    }
  }

  /**
   * Publishes an RFCOMM service and accepts phones on it until {@link #stopRfcomm}.
   * Returns an empty string, or why it could not.
   *
   * <p>Android writes the SDP record itself, RFCOMM channel included, which on Linux has
   * to be asked for by hand. Each phone that connects is bridged to a socket the native
   * side owns, so the handshake there runs on a descriptor exactly as it does on BlueZ's.
   */
  @SuppressWarnings("unused") // Called from native code.
  static String startRfcomm(String uuid, String name) {
    final BluetoothAdapter adapter = bluetoothAdapter();
    if (adapter == null) {
      return "This device has no Bluetooth.";
    }
    synchronized (rfcommSockets) {
      if (rfcommServer != null) {
        return "";
      }
      try {
        if (!adapter.isEnabled()) {
          return "Bluetooth is turned off.";
        }
        rfcommServer =
            adapter.listenUsingRfcommWithServiceRecord(name, UUID.fromString(uuid));
      } catch (SecurityException e) {
        return "The app does not hold the BLUETOOTH_CONNECT permission.";
      } catch (IOException e) {
        return "Could not publish the Bluetooth service: " + e.getMessage();
      }
    }
    watchLinks();
    final BluetoothServerSocket server = rfcommServer;
    final Thread acceptor =
        new Thread(
            () -> {
              for (; ; ) {
                final BluetoothSocket socket;
                try {
                  socket = server.accept();
                } catch (IOException e) {
                  return; // Closed by stopRfcomm.
                }
                bridgeRfcomm(socket);
              }
            },
            "AndroidAuto RFCOMM");
    acceptor.setDaemon(true);
    acceptor.start();
    return "";
  }

  /** Withdraws the service and drops every phone connected to it. */
  @SuppressWarnings("unused") // Called from native code.
  static void stopRfcomm() {
    synchronized (rfcommSockets) {
      if (rfcommServer != null) {
        try {
          rfcommServer.close();
        } catch (IOException ignored) {
          // Closing is all that is wanted.
        }
        rfcommServer = null;
      }
      for (BluetoothSocket socket : rfcommSockets) {
        closeQuietly(socket);
      }
      rfcommSockets.clear();
    }
    dropHeadsetLink();
  }

  // Keeps linkedPhones current.
  private static void watchLinks() {
    final Context app;
    synchronized (lock) {
      app = context;
    }
    if (app == null) {
      return;
    }
    main.post(
        () -> {
          if (aclReceiver != null) {
            return;
          }
          aclReceiver =
              new BroadcastReceiver() {
                @Override
                public void onReceive(Context ignored, Intent intent) {
                  final BluetoothDevice device =
                      intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                  if (device == null) {
                    return;
                  }
                  synchronized (linkedPhones) {
                    linkedPhones.remove(device.getAddress());
                    if (BluetoothDevice.ACTION_ACL_CONNECTED.equals(intent.getAction())) {
                      linkedPhones.add(device.getAddress());
                    }
                  }
                }
              };
          final IntentFilter filter = new IntentFilter();
          filter.addAction(BluetoothDevice.ACTION_ACL_CONNECTED);
          filter.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED);
          if (Build.VERSION.SDK_INT >= 33) {
            app.registerReceiver(aclReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
          } else {
            app.registerReceiver(aclReceiver, filter);
          }
        });
  }

  /**
   * The bonded phone whose Bluetooth link came up most recently, or empty. Links that
   * were up before the service was published are not seen, except through
   * BluetoothDevice.isConnected, which is hidden and tried for what it is worth.
   */
  @SuppressWarnings("unused") // Called from native code.
  static String connectedPhone() {
    synchronized (linkedPhones) {
      String last = "";
      for (String address : linkedPhones) {
        last = address;
      }
      if (!last.isEmpty()) {
        return last;
      }
    }
    final BluetoothAdapter adapter = bluetoothAdapter();
    if (adapter == null) {
      return "";
    }
    try {
      for (BluetoothDevice device : adapter.getBondedDevices()) {
        if (!isPhone(device)) {
          continue;
        }
        final Object connected = BluetoothDevice.class.getMethod("isConnected").invoke(device);
        if (Boolean.TRUE.equals(connected)) {
          return device.getAddress();
        }
      }
    } catch (ReflectiveOperationException | SecurityException e) {
      // Not available to this app; the broadcasts are all there is.
    }
    return "";
  }

  /**
   * Connects to the phone's headset gateway, the way a Bluetooth headset does, and keeps
   * the link until wireless is withdrawn.
   *
   * <p>A phone only starts wireless Android Auto towards a device it has a hands free
   * style link with. A car has one of its own; a phone or a tablet running the head unit
   * has its hands free role switched off, so the phone pairs, reads the service list and
   * then does nothing. Opening the headset profile from this end is what a wireless
   * adapter does in the same position. Headset rather than hands free, because a headset
   * link is up as soon as the socket is, with no AT negotiation to carry through.
   */
  @SuppressWarnings("unused") // Called from native code.
  static void nudgePhone(String address) {
    final BluetoothAdapter adapter = bluetoothAdapter();
    if (adapter == null) {
      return;
    }
    final Thread worker =
        new Thread(
            () -> {
              dropHeadsetLink();
              BluetoothSocket socket = null;
              String linked = "";
              final java.util.List<String> candidates = nudgeCandidates(adapter, address);
              Log.i(TAG, "Opening a headset link to the first of " + candidates);
              for (String candidate : candidates) {
                try {
                  final BluetoothDevice device = adapter.getRemoteDevice(candidate);
                  final BluetoothSocket attempt =
                      device.createRfcommSocketToServiceRecord(HEADSET_GATEWAY);
                  attempt.connect();
                  socket = attempt;
                  linked = candidate;
                  break;
                } catch (IOException | SecurityException | IllegalArgumentException e) {
                  // Out of range, switched off, or not a phone after all.
                  Log.i(TAG, "No headset link to " + candidate + ": " + e.getMessage());
                }
              }
              if (socket == null) {
                return;
              }
              final String address_ = linked;
              Log.i(TAG, "Headset link to " + address_ + " is up");
              synchronized (rfcommSockets) {
                headsetLink = socket;
              }
              // Whatever the gateway says is read and dropped, which keeps the link
              // alive and answers nothing a headset would have to.
              final byte[] buffer = new byte[256];
              try {
                final InputStream in = socket.getInputStream();
                while (in.read(buffer) >= 0) {
                  // Nothing to do with it.
                }
              } catch (IOException ignored) {
                // The link went away.
              }
              Log.i(TAG, "Headset link to " + address_ + " ended");
            },
            "AndroidAuto headset link");
    worker.setDaemon(true);
    worker.start();
  }

  private static final UUID HANDS_FREE_GATEWAY =
      UUID.fromString("0000111f-0000-1000-8000-00805f9b34fb");

  /**
   * Whether a bonded device is a phone worth a headset link.
   *
   * <p>By class first, but only to rule things out: a computer, a headset or a watch is
   * never the phone, even though a Windows PC offers a hands free gateway too. Then by the
   * services Android read at pairing, a headset or hands free gateway being what a link
   * needs. A device whose services were never read is taken on its class, and a Pixel 8
   * Pro calls itself uncategorized, so that counts.
   */
  private static boolean isPhone(BluetoothDevice device) {
    final BluetoothClass type = device.getBluetoothClass();
    final int major = type == null ? BluetoothClass.Device.Major.UNCATEGORIZED
        : type.getMajorDeviceClass();
    if (major != BluetoothClass.Device.Major.PHONE
        && major != BluetoothClass.Device.Major.UNCATEGORIZED
        && major != BluetoothClass.Device.Major.MISC) {
      return false;
    }
    final android.os.ParcelUuid[] uuids = device.getUuids();
    if (uuids == null || uuids.length == 0) {
      return true;
    }
    for (android.os.ParcelUuid uuid : uuids) {
      if (HEADSET_GATEWAY.equals(uuid.getUuid()) || HANDS_FREE_GATEWAY.equals(uuid.getUuid())) {
        return true;
      }
    }
    return false;
  }

  /**
   * Which phones to try a headset link to: the one named, or with no name every bonded
   * phone, the linked ones first. A car does the same when it starts, working down the
   * phones it knows until one answers. A phone that is not there costs a few seconds.
   */
  private static java.util.List<String> nudgeCandidates(BluetoothAdapter adapter, String address) {
    final java.util.List<String> candidates = new java.util.ArrayList<>();
    if (BluetoothAdapter.checkBluetoothAddress(address)) {
      candidates.add(address);
      return candidates;
    }
    synchronized (linkedPhones) {
      candidates.addAll(linkedPhones);
    }
    try {
      for (BluetoothDevice device : adapter.getBondedDevices()) {
        if (isPhone(device) && !candidates.contains(device.getAddress())) {
          candidates.add(device.getAddress());
        }
      }
    } catch (SecurityException ignored) {
      // Without permission there is nothing to try.
    }
    return candidates;
  }

  private static void dropHeadsetLink() {
    final BluetoothSocket socket;
    synchronized (rfcommSockets) {
      socket = headsetLink;
      headsetLink = null;
    }
    if (socket != null) {
      closeQuietly(socket);
    }
  }

  // Copies bytes both ways between the phone and a descriptor the native side reads,
  // until either end goes away, then closes both.
  private static void bridgeRfcomm(BluetoothSocket socket) {
    final String address = socket.getRemoteDevice().getAddress();
    final int fd = nativeRfcommConnection(address);
    if (fd < 0) {
      closeQuietly(socket);
      return;
    }
    synchronized (rfcommSockets) {
      rfcommSockets.add(socket);
    }
    final ParcelFileDescriptor local = ParcelFileDescriptor.adoptFd(fd);
    final Runnable finish =
        () -> {
          closeQuietly(socket);
          try {
            local.close();
          } catch (IOException ignored) {
            // Already gone.
          }
          synchronized (rfcommSockets) {
            rfcommSockets.remove(socket);
          }
        };
    try {
      final InputStream fromPhone = socket.getInputStream();
      final OutputStream toPhone = socket.getOutputStream();
      final InputStream fromNative = new FileInputStream(local.getFileDescriptor());
      final OutputStream toNative = new FileOutputStream(local.getFileDescriptor());
      startPump("AndroidAuto RFCOMM in", fromPhone, toNative, finish);
      startPump("AndroidAuto RFCOMM out", fromNative, toPhone, finish);
    } catch (IOException e) {
      finish.run();
    }
  }

  private static void startPump(
      String name, InputStream from, OutputStream to, Runnable finish) {
    final Thread pump =
        new Thread(
            () -> {
              final byte[] buffer = new byte[4096];
              try {
                for (; ; ) {
                  final int read = from.read(buffer);
                  if (read < 0) {
                    break;
                  }
                  to.write(buffer, 0, read);
                  to.flush();
                }
              } catch (IOException ignored) {
                // Either end closing ends the conversation.
              }
              finish.run();
            },
            name);
    pump.setDaemon(true);
    pump.start();
  }

  private static void closeQuietly(BluetoothSocket socket) {
    try {
      socket.close();
    } catch (IOException ignored) {
      // Closing is all that is wanted.
    }
  }

  /**
   * Every bonded device, one per line: address, name, "0" (Android does not say whether
   * a device is connected without a profile to ask through), and "1" if its class says
   * it is a phone. Empty without Bluetooth or the permission to ask.
   */
  @SuppressWarnings("unused") // Called from native code.
  static String pairedDevices() {
    final BluetoothAdapter adapter = bluetoothAdapter();
    if (adapter == null) {
      return "";
    }
    final StringBuilder listing = new StringBuilder();
    try {
      for (BluetoothDevice device : adapter.getBondedDevices()) {
        final boolean phone = isPhone(device);
        final String name = device.getName();
        listing
            .append(device.getAddress())
            .append('\t')
            .append(name == null ? "" : name.replace('\t', ' ').replace('\n', ' '))
            .append("\t0\t")
            .append(phone ? '1' : '0')
            .append('\n');
      }
    } catch (SecurityException e) {
      return "";
    }
    return listing.toString();
  }

  /**
   * The Wi-Fi network this device is joined to, as one line: interface, SSID, BSSID,
   * IPv4 address, frequency in MHz, tab separated, any of them empty or 0 when unknown.
   *
   * <p>Android hides the SSID and BSSID from an app that does not hold
   * ACCESS_FINE_LOCATION while location is on, and answers with placeholders, which come
   * back as empty: an empty BSSID is the host app's cue to name one itself, a placeholder
   * one would be sent to the phone and silently fail.
   */
  @SuppressWarnings({"unused", "deprecation"}) // Called from native code. getConnectionInfo
  // is deprecated for its callback replacement, and is the one that answers at once.
  static String wifiNetwork() {
    final Context app;
    synchronized (lock) {
      app = context;
    }
    if (app == null) {
      return "";
    }
    final WifiManager wifi = (WifiManager) app.getSystemService(Context.WIFI_SERVICE);
    final ConnectivityManager connectivity =
        (ConnectivityManager) app.getSystemService(Context.CONNECTIVITY_SERVICE);
    String interfaceName = "";
    if (connectivity != null) {
      for (Network network : connectivity.getAllNetworks()) {
        final NetworkCapabilities caps = connectivity.getNetworkCapabilities(network);
        if (caps != null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
          final LinkProperties link = connectivity.getLinkProperties(network);
          if (link != null && link.getInterfaceName() != null) {
            interfaceName = link.getInterfaceName();
          }
        }
      }
    }
    if (wifi == null) {
      return interfaceName + "\t\t\t\t0";
    }
    final WifiInfo info = wifi.getConnectionInfo();
    if (info == null || info.getNetworkId() == -1 && info.getIpAddress() == 0) {
      return interfaceName + "\t\t\t\t0";
    }
    String ssid = info.getSSID();
    if (ssid == null || ssid.equals(WifiManager.UNKNOWN_SSID)) {
      ssid = "";
    } else if (ssid.length() >= 2 && ssid.startsWith("\"") && ssid.endsWith("\"")) {
      ssid = ssid.substring(1, ssid.length() - 1);
    }
    String bssid = info.getBSSID();
    if (bssid == null || bssid.equals("02:00:00:00:00:00")) {
      bssid = "";
    }
    final int ip = info.getIpAddress();
    final String address =
        ip == 0
            ? ""
            : String.format(
                "%d.%d.%d.%d", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
    return interfaceName + "\t" + ssid + "\t" + bssid + "\t" + address + "\t"
        + Math.max(0, info.getFrequency());
  }

  // The hotspot a head unit brings up for the phone, and the thread its callbacks run
  // on, which is not the main thread so that hostWifiNetwork can wait for it there.
  //
  // The callbacks only write the volatile field and never take hotspotLock: the caller
  // holds that lock while it waits for them, and a callback that took it too waited for
  // the caller in turn, until the five seconds ran out.
  private static final Object hotspotLock = new Object();
  private static volatile WifiManager.LocalOnlyHotspotReservation hotspot;
  private static HandlerThread hotspotThread;

  /**
   * Brings up a LocalOnlyHotspot, or keeps the one already up, and describes it as one
   * line: interface, SSID, BSSID, IPv4, passphrase, frequency in MHz (0 when the band is
   * not pinned to one). Empty when it will not start.
   *
   * <p>Android picks the SSID and passphrase itself, which is the point: a head unit with
   * no network to join has one without anybody typing a password, as a car does. It
   * needs the host app to hold ACCESS_FINE_LOCATION, and NEARBY_WIFI_DEVICES from
   * Android 13, with location turned on.
   *
   * <p>Called from the platform thread, which is the main thread, and waits there for up
   * to five seconds the first time. The callbacks go to a thread of their own so that
   * the wait cannot deadlock them.
   */
  @SuppressWarnings({"unused", "deprecation"}) // Called from native code. getSsid is
  // deprecated for getWifiSsid, which is 33 and returns the same name.
  static String hostWifiNetwork() {
    final Context app;
    synchronized (lock) {
      app = context;
    }
    if (app == null || Build.VERSION.SDK_INT < 30) {
      return "";
    }
    final WifiManager wifi = (WifiManager) app.getSystemService(Context.WIFI_SERVICE);
    if (wifi == null) {
      return "";
    }
    synchronized (hotspotLock) {
      if (hotspot == null) {
        if (hotspotThread == null) {
          hotspotThread = new HandlerThread("AndroidAuto hotspot");
          hotspotThread.start();
        }
        final CountDownLatch started = new CountDownLatch(1);
        try {
          wifi.startLocalOnlyHotspot(
              new WifiManager.LocalOnlyHotspotCallback() {
                @Override
                public void onStarted(WifiManager.LocalOnlyHotspotReservation reservation) {
                  hotspot = reservation;
                  started.countDown();
                }

                @Override
                public void onStopped() {
                  hotspot = null;
                }

                @Override
                public void onFailed(int reason) {
                  Log.w(TAG, "The hotspot would not start, reason " + reason);
                  started.countDown();
                }
              },
              new Handler(hotspotThread.getLooper()));
        } catch (SecurityException e) {
          Log.w(TAG, "No permission to start a hotspot: " + e.getMessage());
          return "";
        } catch (IllegalStateException e) {
          // A hotspot this app asked for is already up, or one is being started.
          Log.w(TAG, "The hotspot would not start: " + e.getMessage());
        }
        try {
          started.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
          Thread.currentThread().interrupt();
        }
        if (hotspot == null) {
          return "";
        }
      }
      final WifiManager.LocalOnlyHotspotReservation reservation = hotspot;
      if (reservation == null) {
        return "";
      }
      final SoftApConfiguration config = reservation.getSoftApConfiguration();
      final String ssid = config.getSsid() == null ? "" : config.getSsid();
      final String passphrase = config.getPassphrase() == null ? "" : config.getPassphrase();
      final NetworkInterface face = hotspotInterface();
      final String name = face == null ? "" : face.getName();
      final String address = face == null ? "" : ipv4(face);
      String bssid = "";
      final MacAddress mac = config.getBssid();
      if (mac != null) {
        bssid = mac.toString();
      } else if (face != null) {
        bssid = macFromLinkLocal(face);
      }
      Log.i(
          TAG,
          "Hosting " + ssid + " on " + name + " at " + address + ", bssid "
              + (bssid.isEmpty() ? "unknown" : bssid));
      final int frequency = hotspotFrequency(config);
      return name + "\t" + ssid + "\t" + bssid + "\t" + address + "\t" + passphrase
          + "\t" + frequency;
    }
  }

  // The band the hotspot was configured for, as a frequency: the channel's when one is
  // fixed, the band's first otherwise, 0 when it may use several or Android will not say.
  // Not the channel it got, which only a privileged app may ask for.
  private static int hotspotFrequency(SoftApConfiguration config) {
    if (Build.VERSION.SDK_INT < 31) {
      return 0;
    }
    final android.util.SparseIntArray channels = config.getChannels();
    if (channels == null || channels.size() != 1) {
      return 0;
    }
    final int band = channels.keyAt(0);
    final int channel = channels.valueAt(0);
    if (band == SoftApConfiguration.BAND_2GHZ) {
      return channel > 0 ? 2407 + 5 * channel : 2412;
    }
    if (band == SoftApConfiguration.BAND_5GHZ) {
      return channel > 0 ? 5000 + 5 * channel : 5180;
    }
    return 0;
  }

  /** Takes the hotspot down. */
  @SuppressWarnings("unused") // Called from native code.
  static void releaseWifiNetwork() {
    synchronized (hotspotLock) {
      final WifiManager.LocalOnlyHotspotReservation reservation = hotspot;
      hotspot = null;
      if (reservation != null) {
        reservation.close();
      }
    }
  }

  // The interface the hotspot runs on. Android names it differently per device
  // (swlan0, wlan1, ap0), so it is found by what it is: up, with a private IPv4 address,
  // and not the interface a joined network or mobile data uses.
  private static NetworkInterface hotspotInterface() {
    try {
      for (NetworkInterface face : Collections.list(NetworkInterface.getNetworkInterfaces())) {
        final String name = face.getName();
        if (!face.isUp() || face.isLoopback() || name.equals("wlan0")
            || name.startsWith("rmnet") || name.startsWith("dummy")) {
          continue;
        }
        if (!ipv4(face).isEmpty()
            && (name.startsWith("swlan") || name.startsWith("wlan") || name.startsWith("ap"))) {
          return face;
        }
      }
    } catch (java.net.SocketException ignored) {
      // No interfaces to look at.
    }
    return null;
  }

  private static String ipv4(NetworkInterface face) {
    for (InetAddress address : Collections.list(face.getInetAddresses())) {
      if (address instanceof Inet4Address && !address.isLoopbackAddress()) {
        return address.getHostAddress();
      }
    }
    return "";
  }

  /**
   * An interface's MAC, recovered from its IPv6 link-local address, or empty.
   *
   * <p>An access point's BSSID is its interface's MAC, and apps have not been told MACs
   * since Android 11: not through NetworkInterface, sysfs or netlink. Addresses they
   * still get, and a link-local address built the EUI-64 way carries the MAC inside it,
   * with ff:fe in the middle and the universal/local bit flipped. Android builds the
   * hotspot's that way on the device tested. One built for privacy carries nothing,
   * which shows as no ff:fe, and then this says nothing rather than guess.
   */
  private static String macFromLinkLocal(NetworkInterface face) {
    for (InetAddress address : Collections.list(face.getInetAddresses())) {
      if (!(address instanceof java.net.Inet6Address) || !address.isLinkLocalAddress()) {
        continue;
      }
      final byte[] b = address.getAddress();
      if ((b[11] & 0xff) != 0xff || (b[12] & 0xff) != 0xfe) {
        continue;
      }
      return String.format(
          "%02x:%02x:%02x:%02x:%02x:%02x",
          (b[8] & 0xff) ^ 0x02, b[9] & 0xff, b[10] & 0xff, b[13] & 0xff, b[14] & 0xff,
          b[15] & 0xff);
    }
    return "";
  }

  private static native int nativeRfcommConnection(String address);

  private static native void nativeUsbDevice(int fd, boolean accessory, String name);

  private static native void nativeAttach(Context context);

  private static native void nativeSurfaceAvailable(long handle, Surface surface);

  private static native void nativeSurfaceCleanup(long handle);
}
