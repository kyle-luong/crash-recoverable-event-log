#ifndef EVENTLOG_JSON_H_
#define EVENTLOG_JSON_H_

#include <string>
#include <string_view>

namespace eventlog {

// Escapes a string for a JSON string literal. Two commands emit JSON (`verify --json`
// and `bench --out`), and both embed strings this project did not choose -- a path, a
// filesystem name, a CPU model line -- so neither may assume the bytes are safe.
inline std::string JsonEscape(std::string_view in) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out += kHexDigits[(static_cast<unsigned char>(c) >> 4) & 0xF];
          out += kHexDigits[static_cast<unsigned char>(c) & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace eventlog

#endif  // EVENTLOG_JSON_H_
