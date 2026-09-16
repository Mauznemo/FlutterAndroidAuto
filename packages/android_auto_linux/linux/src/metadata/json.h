// A very small JSON writer, for the one thing in this plugin that needs one.
//
// M9 carries structured news from the phone to the host app: a turn instruction with a
// distance and a lane diagram, a track with its artwork, a list of calls. None of that
// fits the flat scalars the rest of the C ABI passes, and adding a JSON library to a
// project whose only other dependencies are the ones the protocol forces on it would be
// out of proportion. So one string per update crosses the boundary, built here.
//
// Two rules, both of which exist because of what M8 learned about absent fields:
//
//   - A field the phone did not send is not written at all. Not written as zero, not
//     written as an empty string. A track with no album and a track on an album called
//     "" are different, and only the caller can tell which it has.
//   - Pictures are base64, inline. Album art and turn icons are tens of kilobytes and
//     arrive once per track or once per turn, so the third they gain by being encoded
//     costs nothing measurable and buys one owner, one free, and one snapshot getter
//     that returns everything rather than everything except the pictures. They are
//     encoded once, where they arrive, rather than on every update that carries them
//     along: see the base64 fields in metadata_state.h.
//
// Not a parser. Nothing native reads JSON; Dart does that with dart:convert.

#ifndef ANDROID_AUTO_LINUX_METADATA_JSON_H_
#define ANDROID_AUTO_LINUX_METADATA_JSON_H_

#include <cstdint>
#include <string>
#include <vector>

namespace aa {

// One JSON object, built a field at a time. Every Add returns *this so a message can be
// written as one expression per field.
class Json {
 public:
  Json& AddString(const char* key, const std::string& value);
  Json& AddInt(const char* key, int64_t value);
  Json& AddDouble(const char* key, double value);
  Json& AddBool(const char* key, bool value);
  // An already built object or array, such as one from Done() or JsonArray().
  Json& AddRaw(const char* key, const std::string& json);

  // Whether anything has been written. An update that turns out to carry nothing this
  // head unit understands is worth dropping rather than emitting as `{}`.
  bool empty() const { return body_.empty(); }

  // The finished object, braces included. The builder is left usable but nobody does.
  std::string Done() const;

 private:
  void Key(const char* key);

  std::string body_;
};

// `["a","b"]` from a list of already quoted or already built elements.
std::string JsonArray(const std::vector<std::string>& elements);

// One JSON string literal, quotes and escaping included. For building array elements.
std::string JsonString(const std::string& value);

// Standard base64, no line breaks. Empty for an empty input.
std::string Base64(const uint8_t* data, size_t size);

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_METADATA_JSON_H_
