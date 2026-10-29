// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_jni.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace aa {
namespace android {
namespace {

constexpr const char* kTag = "AndroidAuto";
constexpr const char* kPluginClass = "io/github/mauznemo/android_auto/AndroidAutoPlugin";

JavaVM* g_vm = nullptr;
jclass g_plugin = nullptr;
jmethodID g_create_surface = nullptr;
jmethodID g_resize_surface = nullptr;
jmethodID g_release_surface = nullptr;
jmethodID g_start_usb = nullptr;
jmethodID g_stop_usb = nullptr;
jmethodID g_rescan_usb = nullptr;
jmethodID g_release_usb = nullptr;
jobject g_context = nullptr;

// Which listeners are live. Java posts resizes to the main thread, so a surface can
// arrive for a listener that has been released in between; it is dropped rather than
// handed to an object that has gone.
std::mutex g_listeners_mutex;
std::set<SurfaceListener*> g_listeners;

std::mutex g_usb_mutex;
UsbDeviceHandler g_usb_handler;

pthread_key_t g_detach_key;

// Threads the core attaches to the VM are detached again when they exit, or the VM
// refuses to shut the thread down cleanly.
void DetachOnExit(void*) {
  if (g_vm != nullptr) {
    g_vm->DetachCurrentThread();
  }
}

// The environment for the calling thread, attaching it if need be.
JNIEnv* Env() {
  if (g_vm == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    return nullptr;
  }
  pthread_setspecific(g_detach_key, env);
  return env;
}

void ClearException(JNIEnv* env, const char* what) {
  if (env->ExceptionCheck()) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Java threw during %s", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

SurfaceListener* Live(jlong handle) {
  auto* listener = reinterpret_cast<SurfaceListener*>(handle);
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  return g_listeners.count(listener) != 0 ? listener : nullptr;
}

// Android sends a process's stdout and stderr nowhere. aasdk logs to both, and so do
// the core's own diagnostics, so they are copied into logcat line by line rather than
// every call site learning about Android.
void RedirectStdioToLogcat() {
  static int fds[2];
  if (pipe(fds) != 0) {
    return;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  std::thread([]() {
    std::string line;
    char buffer[512];
    for (;;) {
      const ssize_t n = read(fds[0], buffer, sizeof(buffer));
      if (n <= 0) {
        return;
      }
      for (ssize_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') {
          __android_log_write(ANDROID_LOG_INFO, kTag, line.c_str());
          line.clear();
        } else {
          line.push_back(buffer[i]);
        }
      }
    }
  }).detach();
}

}  // namespace

int64_t CreateSurface(SurfaceListener* listener, int32_t width, int32_t height) {
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    g_listeners.insert(listener);
  }
  const jlong id = env->CallStaticLongMethod(g_plugin, g_create_surface,
                                             reinterpret_cast<jlong>(listener), width, height);
  ClearException(env, "createSurface");
  if (id < 0) {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    g_listeners.erase(listener);
  }
  return id;
}

void ResizeSurface(SurfaceListener* listener, int32_t width, int32_t height) {
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return;
  }
  env->CallStaticVoidMethod(g_plugin, g_resize_surface, reinterpret_cast<jlong>(listener),
                            width, height);
  ClearException(env, "resizeSurface");
}

void ReleaseSurface(SurfaceListener* listener) {
  JNIEnv* env = Env();
  if (env != nullptr && g_plugin != nullptr) {
    env->CallStaticVoidMethod(g_plugin, g_release_surface,
                              reinterpret_cast<jlong>(listener));
    ClearException(env, "releaseSurface");
  }
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  g_listeners.erase(listener);
}

void StartUsbDiscovery(UsbDeviceHandler handler) {
  {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    g_usb_handler = std::move(handler);
  }
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return;
  }
  env->CallStaticVoidMethod(g_plugin, g_start_usb);
  ClearException(env, "startUsb");
}

void StopUsbDiscovery() {
  {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    g_usb_handler = nullptr;
  }
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return;
  }
  env->CallStaticVoidMethod(g_plugin, g_stop_usb);
  ClearException(env, "stopUsb");
}

void RescanUsb() {
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return;
  }
  env->CallStaticVoidMethod(g_plugin, g_rescan_usb);
  ClearException(env, "rescanUsb");
}

void ReleaseUsbDevice(int fd) {
  JNIEnv* env = Env();
  if (env == nullptr || g_plugin == nullptr) {
    return;
  }
  env->CallStaticVoidMethod(g_plugin, g_release_usb, static_cast<jint>(fd));
  ClearException(env, "releaseUsb");
}

// Called by Java with a device it has opened.
void OfferUsbDevice(int fd, bool accessory, const std::string& name) {
  UsbDeviceHandler handler;
  {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    handler = g_usb_handler;
  }
  if (handler) {
    handler(fd, accessory, name);
  } else {
    // Nobody wants it any more, so it goes straight back.
    ReleaseUsbDevice(fd);
  }
}

}  // namespace android
}  // namespace aa

using aa::android::g_context;
using aa::android::g_create_surface;
using aa::android::g_plugin;
using aa::android::g_release_surface;
using aa::android::g_release_usb;
using aa::android::g_rescan_usb;
using aa::android::g_resize_surface;
using aa::android::g_start_usb;
using aa::android::g_stop_usb;
using aa::android::g_vm;

extern "C" {

// Runs when Java loads the library, on a thread whose class loader can see the plugin's
// classes. Looking the class up later, from a thread the core started, would go through
// the system class loader and find nothing.
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  pthread_key_create(&aa::android::g_detach_key, aa::android::DetachOnExit);
  aa::android::RedirectStdioToLogcat();

  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass local = env->FindClass(aa::android::kPluginClass);
  if (local == nullptr) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, aa::android::kTag, "Cannot find %s",
                        aa::android::kPluginClass);
    return JNI_VERSION_1_6;
  }
  g_plugin = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  g_create_surface = env->GetStaticMethodID(g_plugin, "createSurface", "(JII)J");
  g_resize_surface = env->GetStaticMethodID(g_plugin, "resizeSurface", "(JII)V");
  g_release_surface = env->GetStaticMethodID(g_plugin, "releaseSurface", "(J)V");
  g_start_usb = env->GetStaticMethodID(g_plugin, "startUsb", "()V");
  g_stop_usb = env->GetStaticMethodID(g_plugin, "stopUsb", "()V");
  g_rescan_usb = env->GetStaticMethodID(g_plugin, "rescanUsb", "()V");
  g_release_usb = env->GetStaticMethodID(g_plugin, "releaseUsb", "(I)V");
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_io_github_mauznemo_android_1auto_AndroidAutoPlugin_nativeAttach(
    JNIEnv* env, jclass, jobject context) {
  if (g_context != nullptr) {
    env->DeleteGlobalRef(g_context);
  }
  g_context = context == nullptr ? nullptr : env->NewGlobalRef(context);
}

JNIEXPORT void JNICALL
Java_io_github_mauznemo_android_1auto_AndroidAutoPlugin_nativeSurfaceAvailable(
    JNIEnv* env, jclass, jlong handle, jobject surface) {
  aa::android::SurfaceListener* listener = aa::android::Live(handle);
  if (listener == nullptr || surface == nullptr) {
    return;
  }
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window != nullptr) {
    listener->OnSurfaceAvailable(window);
  }
}

JNIEXPORT void JNICALL
Java_io_github_mauznemo_android_1auto_AndroidAutoPlugin_nativeSurfaceCleanup(JNIEnv*,
                                                                            jclass,
                                                                            jlong handle) {
  if (aa::android::SurfaceListener* listener = aa::android::Live(handle)) {
    listener->OnSurfaceCleanup();
  }
}

JNIEXPORT void JNICALL Java_io_github_mauznemo_android_1auto_AndroidAutoPlugin_nativeUsbDevice(
    JNIEnv* env, jclass, jint fd, jboolean accessory, jstring name) {
  std::string label;
  if (name != nullptr) {
    const char* chars = env->GetStringUTFChars(name, nullptr);
    label = chars == nullptr ? "" : chars;
    if (chars != nullptr) {
      env->ReleaseStringUTFChars(name, chars);
    }
  }
  aa::android::OfferUsbDevice(fd, accessory == JNI_TRUE, label);
}

}  // extern "C"
