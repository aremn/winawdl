// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <string_view>

inline std::string jsonString(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      output += '\\';
      output += char(byte);
    } else if (byte < 0x20) {
      output += "\\u00";
      output += hex[byte >> 4];
      output += hex[byte & 15];
    } else {
      output += char(byte);
    }
  }
  output += '"';
  return output;
}
