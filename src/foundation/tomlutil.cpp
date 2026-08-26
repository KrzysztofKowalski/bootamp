// foundation/tomlutil.cpp — shared TOML helpers for config/providers.
//
// Faithful port of cliamp/internal/tomlutil (sections.go + unquote.go).
#include "foundation/tomlutil.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bootamp::foundation {

namespace {

// trim_space removes leading/trailing ASCII whitespace (space, tab, CR, LF,
// VT, FF). Matches Go's strings.TrimSpace for the ASCII range used by TOML
// line preprocessing.
std::string_view trim_space(std::string_view s) {
  constexpr auto is_ws = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
  };
  std::size_t b = 0, e = s.size();
  while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// append_utf8 appends the UTF-8 encoding of cp to out. Replaces invalid
// codepoints with the replacement byte sequence cliamp would never emit here.
void append_utf8(std::string& out, unsigned long cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// hex_value returns 0-15 for a hex digit, or -1 if not hex.
int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// try_unquote interprets Go-style escape sequences inside a double-quoted
// string (without the surrounding quotes). Returns false on any invalid
// escape so the caller can fall back to the naive strip, matching cliamp's
// strconv.Unquote fallback path.
bool try_unquote(std::string_view inner, std::string& out) {
  out.clear();
  std::size_t i = 0;
  while (i < inner.size()) {
    char c = inner[i];
    if (c != '\\') {
      out.push_back(c);
      ++i;
      continue;
    }
    // escape sequence
    if (i + 1 >= inner.size()) return false;  // dangling backslash
    char e = inner[i + 1];
    i += 2;
    switch (e) {
      case 'a': out.push_back('\a'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'v': out.push_back('\v'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case 'x': {
        if (i + 2 > inner.size()) return false;
        int hi = hex_value(inner[i]);
        int lo = hex_value(inner[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        break;
      }
      case 'u': {
        if (i + 4 > inner.size()) return false;
        unsigned long cp = 0;
        for (int k = 0; k < 4; ++k) {
          int hv = hex_value(inner[i + k]);
          if (hv < 0) return false;
          cp = (cp << 4) | static_cast<unsigned long>(hv);
        }
        append_utf8(out, cp);
        i += 4;
        break;
      }
      case 'U': {
        if (i + 8 > inner.size()) return false;
        unsigned long cp = 0;
        for (int k = 0; k < 8; ++k) {
          int hv = hex_value(inner[i + k]);
          if (hv < 0) return false;
          cp = (cp << 4) | static_cast<unsigned long>(hv);
        }
        append_utf8(out, cp);
        i += 8;
        break;
      }
      default:
        // Unknown escape (e.g. "\z") — strconv.Unquote fails; caller falls
        // back to the naive strip.
        return false;
    }
  }
  return true;
}

}  // namespace

std::string unquote(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string out;
    if (try_unquote(s.substr(1, s.size() - 2), out)) {
      return out;
    }
    // Fall back to naive strip if Unquote fails, exactly like cliamp.
    return std::string{s.substr(1, s.size() - 2)};
  }
  return std::string{s};
}

void parse_sections(std::string_view data, std::string_view section,
                    const std::function<void(const TomlFields&)>& emit) {
  std::string sec{section};
  std::span<const std::string> secs{&sec, 1};
  parse_named_sections(data, secs,
                       [&emit](std::string_view, const TomlFields& f) { emit(f); });
}

void parse_named_sections(
    std::string_view data, std::span<const std::string> sections,
    const std::function<void(std::string_view section, const TomlFields&)>& emit) {
  // Build the header lookup table: "[[<name>]]" -> name.
  std::unordered_map<std::string, std::string> headers;
  headers.reserve(sections.size());
  for (const std::string& s : sections) {
    headers["[[" + s + "]]"] = s;
  }

  std::string cur;        // current section name (empty when none)
  bool in_section = false;
  TomlFields fields;      // valid only while in_section == true

  auto flush = [&] {
    if (in_section) {
      emit(cur, fields);
      in_section = false;
      fields.clear();
      cur.clear();
    }
  };

  // Walk the data line by line, splitting on '\n' exactly like
  // strings.SplitSeq(string(data), "\n").
  std::size_t pos = 0;
  while (pos <= data.size()) {
    std::size_t nl = data.find('\n', pos);
    std::string_view raw_line;
    if (nl == std::string_view::npos) {
      raw_line = data.substr(pos);
      pos = data.size() + 1;  // past end, terminates loop
    } else {
      raw_line = data.substr(pos, nl - pos);
      pos = nl + 1;
    }

    std::string_view line = trim_space(raw_line);
    if (line.empty() || line.front() == '#') continue;

    // Exact-match a recognized [[section]] header.
    if (auto it = headers.find(std::string{line}); it != headers.end()) {
      flush();
      in_section = true;
      fields.clear();
      cur = it->second;
      continue;
    }
    // Any other [[...]] header: flush the current recognized section so its
    // fields cannot leak into the next one, then drop to "no section".
    if (line.size() >= 4 && line.substr(0, 2) == "[[" && line.substr(line.size() - 2) == "]]") {
      flush();
      in_section = false;
      fields.clear();
      cur.clear();
      continue;
    }
    if (!in_section) continue;

    // key = value  (split at the FIRST '=', like strings.Cut).
    std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = trim_space(line.substr(0, eq));
    std::string_view val = trim_space(line.substr(eq + 1));
    fields[std::string{key}] = unquote(val);
  }
  flush();
}

}  // namespace bootamp::foundation