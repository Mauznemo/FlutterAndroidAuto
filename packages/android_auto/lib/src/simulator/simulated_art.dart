// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' show Color;

/// A cover image for a simulated track: a diagonal gradient between [colors] with a
/// pale disc in the middle, as a PNG, the format a phone most often sends.
///
/// Encoded here in plain Dart rather than drawn with `Picture.toImage`, because that
/// needs the engine's rasteriser and never completes under `flutter test`, which then
/// waits on it forever. A host app's own tests run against this simulator too.
Uint8List simulatedCoverArt(List<Color> colors, {int size = 128}) {
  final from = colors.first;
  final to = colors.last;
  // One filter byte, then RGB, per row.
  final stride = 1 + size * 3;
  final raw = Uint8List(stride * size);
  final centre = (size - 1) / 2;
  for (var y = 0; y < size; y++) {
    raw[y * stride] = 0;
    for (var x = 0; x < size; x++) {
      final t = (x + y) / (2 * (size - 1));
      var r = from.r + (to.r - from.r) * t;
      var g = from.g + (to.g - from.g) * t;
      var b = from.b + (to.b - from.b) * t;
      final distance = math.sqrt(math.pow(x - centre, 2) + math.pow(y - centre, 2));
      if (distance < size * 0.06) {
        r = g = b = 0.9;
      } else if (distance < size * 0.3) {
        r += (1 - r) * 0.2;
        g += (1 - g) * 0.2;
        b += (1 - b) * 0.2;
      }
      final offset = y * stride + 1 + x * 3;
      raw[offset] = (r * 255).round().clamp(0, 255);
      raw[offset + 1] = (g * 255).round().clamp(0, 255);
      raw[offset + 2] = (b * 255).round().clamp(0, 255);
    }
  }

  final header = ByteData(13)
    ..setUint32(0, size)
    ..setUint32(4, size)
    ..setUint8(8, 8) // bits per channel
    ..setUint8(9, 2) // RGB
    ..setUint8(10, 0)
    ..setUint8(11, 0)
    ..setUint8(12, 0);
  final png = BytesBuilder()
    ..add(const [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    ..add(_chunk('IHDR', header.buffer.asUint8List()))
    ..add(_chunk('IDAT', _zlibStored(raw)))
    ..add(_chunk('IEND', Uint8List(0)));
  return png.toBytes();
}

/// [data] wrapped in a zlib stream of uncompressed blocks. Larger than it could be,
/// which does not matter for a picture this size, and needs no compressor.
Uint8List _zlibStored(Uint8List data) {
  final out = BytesBuilder()..add(const [0x78, 0x01]);
  var offset = 0;
  do {
    final length = math.min(0xFFFF, data.length - offset);
    final last = offset + length >= data.length;
    out
      ..addByte(last ? 1 : 0)
      ..addByte(length & 0xFF)
      ..addByte(length >> 8)
      ..addByte(~length & 0xFF)
      ..addByte((~length >> 8) & 0xFF)
      ..add(Uint8List.sublistView(data, offset, offset + length));
    offset += length;
  } while (offset < data.length);
  var a = 1;
  var b = 0;
  for (final byte in data) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  out.add((ByteData(4)..setUint32(0, (b << 16) | a)).buffer.asUint8List());
  return out.toBytes();
}

Uint8List _chunk(String type, Uint8List data) {
  final typeBytes = Uint8List.fromList(type.codeUnits);
  final crc = _crc32([...typeBytes, ...data]);
  final chunk = ByteData(12 + data.length)..setUint32(0, data.length);
  final bytes = chunk.buffer.asUint8List()
    ..setRange(4, 8, typeBytes)
    ..setRange(8, 8 + data.length, data);
  chunk.setUint32(8 + data.length, crc);
  return bytes;
}

final List<int> _crcTable = List.generate(256, (n) {
  var c = n;
  for (var k = 0; k < 8; k++) {
    c = (c & 1) != 0 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  }
  return c;
});

int _crc32(List<int> bytes) {
  var crc = 0xFFFFFFFF;
  for (final byte in bytes) {
    crc = _crcTable[(crc ^ byte) & 0xFF] ^ (crc >>> 8);
  }
  return crc ^ 0xFFFFFFFF;
}
