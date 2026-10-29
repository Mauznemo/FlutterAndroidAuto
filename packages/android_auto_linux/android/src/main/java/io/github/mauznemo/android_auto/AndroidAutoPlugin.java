// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.mauznemo.android_auto;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.NonNull;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
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
 * UsbManager}, which is the only way an app gets at a phone on the cable.
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

  static {
    System.loadLibrary("android_auto_native");
  }

  @Override
  public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
    synchronized (lock) {
      textures = binding.getTextureRegistry();
      context = binding.getApplicationContext();
    }
    nativeAttach(binding.getApplicationContext());
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

  private static native void nativeUsbDevice(int fd, boolean accessory, String name);

  private static native void nativeAttach(Context context);

  private static native void nativeSurfaceAvailable(long handle, Surface surface);

  private static native void nativeSurfaceCleanup(long handle);
}
