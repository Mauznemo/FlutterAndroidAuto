// SPDX-License-Identifier: GPL-3.0-or-later
#include "include/android_auto_linux/android_auto_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include "src/present/texture_registry.h"

// This plugin exists for one reason: to run at startup and capture the
// FlTextureRegistrar.
//
// Everything else the plugin does goes through the C ABI in src/aa_core.h, called
// directly from Dart over FFI, because touch events and per frame signalling through a
// method channel would add latency and garbage for no benefit. But a pure ffiPlugin
// never receives an FlPluginRegistrar, and without one there is no texture registrar
// and therefore no way to put video on screen. Hence the pluginClass entry in
// pubspec.yaml and this file.
//
// Do not add a method channel here without a reason that FFI cannot serve.

#define ANDROID_AUTO_LINUX_PLUGIN(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), android_auto_linux_plugin_get_type(), \
                              AndroidAutoLinuxPlugin))

struct _AndroidAutoLinuxPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(AndroidAutoLinuxPlugin, android_auto_linux_plugin, g_object_get_type())

static void android_auto_linux_plugin_class_init(AndroidAutoLinuxPluginClass* klass) {}

static void android_auto_linux_plugin_init(AndroidAutoLinuxPlugin* self) {}

void android_auto_linux_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  AndroidAutoLinuxPlugin* plugin = ANDROID_AUTO_LINUX_PLUGIN(
      g_object_new(android_auto_linux_plugin_get_type(), nullptr));

  aa::SetFlutterRegistrar(fl_plugin_registrar_get_texture_registrar(registrar),
                          fl_plugin_registrar_get_view(registrar));

  g_object_unref(plugin);
}
