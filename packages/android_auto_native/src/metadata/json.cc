// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.h"

#include <cmath>
#include <cstdio>

namespace aa {
namespace {

constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Escapes what JSON requires and nothing else. The strings here are road names, track
// titles and caller ids, so they are UTF-8 with anything in them; the bytes above 0x1f
// are passed through unchanged, which is what keeps "Hauptstraße" readable on the other
// side rather than turning it into escapes.
void AppendEscaped(const std::string& value, std::string* out) {
  out->push_back('"');
  for (const char character : value) {
    switch (character) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04x", character);
          out->append(escape);
        } else {
          out->push_back(character);
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendBase64(const uint8_t* data, size_t size, std::string* out) {
  out->reserve(out->size() + (size + 2) / 3 * 4);
  size_t index = 0;
  while (index + 2 < size) {
    const uint32_t triple =
        (static_cast<uint32_t>(data[index]) << 16) |
        (static_cast<uint32_t>(data[index + 1]) << 8) | data[index + 2];
    out->push_back(kBase64[(triple >> 18) & 0x3f]);
    out->push_back(kBase64[(triple >> 12) & 0x3f]);
    out->push_back(kBase64[(triple >> 6) & 0x3f]);
    out->push_back(kBase64[triple & 0x3f]);
    index += 3;
  }
  const size_t remaining = size - index;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(data[index]) << 16;
    out->push_back(kBase64[(triple >> 18) & 0x3f]);
    out->push_back(kBase64[(triple >> 12) & 0x3f]);
    out->append("==");
  } else if (remaining == 2) {
    const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                            (static_cast<uint32_t>(data[index + 1]) << 8);
    out->push_back(kBase64[(triple >> 18) & 0x3f]);
    out->push_back(kBase64[(triple >> 12) & 0x3f]);
    out->push_back(kBase64[(triple >> 6) & 0x3f]);
    out->push_back('=');
  }
}

}  // namespace

void Json::Key(const char* key) {
  if (!body_.empty()) {
    body_.push_back(',');
  }
  AppendEscaped(key, &body_);
  body_.push_back(':');
}

Json& Json::AddString(const char* key, const std::string& value) {
  Key(key);
  AppendEscaped(value, &body_);
  return *this;
}

Json& Json::AddInt(const char* key, int64_t value) {
  Key(key);
  body_.append(std::to_string(value));
  return *this;
}

Json& Json::AddDouble(const char* key, double value) {
  // JSON has no notion of a NaN or an infinity, and the callers here would only produce
  // one from a field they should not have written at all, so it becomes null rather
  // than a token no parser accepts.
  Key(key);
  if (std::isfinite(value)) {
    char formatted[32];
    snprintf(formatted, sizeof(formatted), "%.6g", value);
    body_.append(formatted);
  } else {
    body_.append("null");
  }
  return *this;
}

Json& Json::AddBool(const char* key, bool value) {
  Key(key);
  body_.append(value ? "true" : "false");
  return *this;
}

Json& Json::AddRaw(const char* key, const std::string& json) {
  if (json.empty()) {
    return *this;
  }
  Key(key);
  body_.append(json);
  return *this;
}

std::string Json::Done() const { return "{" + body_ + "}"; }

std::string JsonArray(const std::vector<std::string>& elements) {
  std::string out("[");
  for (size_t index = 0; index < elements.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    out.append(elements[index]);
  }
  out.push_back(']');
  return out;
}

std::string JsonString(const std::string& value) {
  std::string out;
  AppendEscaped(value, &out);
  return out;
}

std::string Base64(const uint8_t* data, size_t size) {
  std::string out;
  if (data != nullptr && size != 0) {
    AppendBase64(data, size, &out);
  }
  return out;
}

}  // namespace aa
