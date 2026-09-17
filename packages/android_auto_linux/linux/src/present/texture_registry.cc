// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_registry.h"

namespace aa {
namespace {

// Written once on the platform thread during plugin registration, read afterwards.
// Plain pointers are enough: registration happens before the Dart isolate that would
// read them exists.
FlTextureRegistrar* g_texture_registrar = nullptr;
FlView* g_view = nullptr;

}  // namespace

void SetFlutterRegistrar(FlTextureRegistrar* registrar, FlView* view) {
  g_texture_registrar = registrar;
  g_view = view;
}

FlTextureRegistrar* GetTextureRegistrar() { return g_texture_registrar; }

FlView* GetFlutterView() { return g_view; }

}  // namespace aa
