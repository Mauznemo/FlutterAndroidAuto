import 'dart:ffi';
import 'dart:io';

import 'bindings/aa_core_bindings.dart';

/// Loads the native head unit core and exposes its bindings.
///
/// The core lives inside `libandroid_auto_linux_plugin.so`, the same shared object
/// Flutter already loaded to run the plugin's GTK registration. Opening it again by
/// name is cheap: `dlopen` returns a handle to the copy that is already mapped.
class AaLibrary {
  AaLibrary._(this.bindings);

  /// The generated FFI bindings.
  final AaCoreBindings bindings;

  static AaLibrary? _instance;

  /// The process wide instance, loaded on first use.
  ///
  /// Throws [UnsupportedError] off Linux and [AaLibraryLoadException] if the shared
  /// object cannot be found, which in practice means the plugin was not bundled.
  static AaLibrary get instance => _instance ??= AaLibrary._(AaCoreBindings(_open()));

  static DynamicLibrary _open() {
    if (!Platform.isLinux) {
      throw UnsupportedError('android_auto_linux only supports Linux.');
    }
    const name = 'libandroid_auto_linux_plugin.so';
    try {
      return DynamicLibrary.open(name);
    } on ArgumentError catch (error) {
      // Fall back to the running process. If Flutter loaded the plugin with its symbols
      // globally visible, this finds them even when the file itself is not on the
      // loader's search path.
      try {
        final process = DynamicLibrary.process();
        process.lookup<NativeFunction<Void Function(Pointer<Char>)>>('aa_string_free');
        return process;
      } on ArgumentError {
        throw AaLibraryLoadException(name, error.toString());
      }
    }
  }
}

/// Thrown when the native core cannot be loaded.
class AaLibraryLoadException implements Exception {
  /// Creates an exception naming the library that failed to load.
  const AaLibraryLoadException(this.libraryName, this.details);

  /// The shared object that could not be opened.
  final String libraryName;

  /// The loader's own error text.
  final String details;

  @override
  String toString() =>
      'Could not load $libraryName. The android_auto_linux plugin is probably not '
      'bundled with this application. Details: $details';
}
