// playlist/playlist.cpp — port of cliamp/playlist/playlist.go.
//
// Thread-safe ordered track list with shuffle/repeat/queue, Fisher-Yates
// shuffle, snapshot/restore, URL classifiers and TrackFromPath. Internal
// helpers (URL parsing, codepage mojibake re-decoding, RNG) live in the
// anonymous namespace and are not part of the public interface.
#include "playlist/playlist.hpp"
#include "playlist/tags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bootamp::playlist {
namespace {

// ============================================================================
// RNG — stand-in for Go math/rand (global source). Thread-local mt19937.
// ============================================================================
std::mt19937& rng() {
  static thread_local std::mt19937 gen{std::random_device{}()};
  return gen;
}

// rand.Intn(n): uniform in [0, n-1]; n >= 1.
int rng_int(int n) {
  return std::uniform_int_distribution<int>(0, n - 1)(rng());
}

// ============================================================================
// UTF-8 — Go unicode/utf8 semantics.
// ============================================================================
struct DecodedRune {
  char32_t r;      // decoded code point, U+FFFD (RuneError) on malformed input
  int      size;   // bytes consumed (1 for malformed input)
};

// Go utf8.DecodeRuneInString: RuneError with size 1 on malformed sequences.
DecodedRune decode_rune(std::string_view s, size_t pos) {
  if (pos >= s.size()) {
    return {0, 0};
  }
  const unsigned char b0 = static_cast<unsigned char>(s[pos]);
  if (b0 < 0x80) {
    return {static_cast<char32_t>(b0), 1};
  }
  int len;
  char32_t cp;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    return {0xFFFD, 1};
  }
  if (pos + static_cast<size_t>(len) > s.size()) {
    return {0xFFFD, 1};
  }
  for (int k = 1; k < len; ++k) {
    const unsigned char bk = static_cast<unsigned char>(s[pos + static_cast<size_t>(k)]);
    if ((bk & 0xC0) != 0x80) {
      return {0xFFFD, 1};
    }
    cp = (cp << 6) | (bk & 0x3F);
  }
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
      (len == 4 && cp < 0x10000)) {
    return {0xFFFD, 1};  // overlong
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return {0xFFFD, 1};  // surrogate
  }
  if (cp > 0x10FFFF) {
    return {0xFFFD, 1};
  }
  return {cp, len};
}

// ============================================================================
// String helpers (Go strings package semantics).
// ============================================================================
bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.substr(s.size() - suffix.size()) == suffix;
}

// Go strings.ToLower applied to an ASCII hostname.
std::string lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Go strings.TrimPrefix(host, "www.") then strings.TrimPrefix(host, "m.").
std::string_view trim_host_prefix(std::string_view host) {
  if (starts_with(host, "www.")) {
    host.remove_prefix(4);
  }
  if (starts_with(host, "m.")) {
    host.remove_prefix(2);
  }
  return host;
}

// Go unicode.IsSpace subset relevant to strings.TrimSpace.
bool is_space_rune(char32_t c) {
  switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x20: case 0x85: case 0xA0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000:
      return true;
    default:
      return false;
  }
}

// Go strings.TrimSpace: trim leading/trailing Unicode whitespace.
std::string trim_space(std::string_view s) {
  size_t begin = 0;
  const size_t end_total = s.size();
  while (begin < end_total) {
    const DecodedRune d = decode_rune(s, begin);
    if (!is_space_rune(d.r)) {
      break;
    }
    begin += static_cast<size_t>(d.size);
  }
  size_t end = end_total;
  while (end > begin) {
    // Walk back over continuation bytes to the start of the final rune.
    size_t start = end;
    while (start > begin) {
      --start;
      if ((static_cast<unsigned char>(s[start]) & 0xC0) != 0x80) {
        break;
      }
    }
    const DecodedRune d = decode_rune(s, start);
    if (d.size <= 0 || static_cast<size_t>(d.size) != end - start) {
      break;  // malformed tail rune — not whitespace (matches Go RuneError)
    }
    if (!is_space_rune(d.r)) {
      break;
    }
    end = start;
  }
  return std::string(s.substr(begin, end - begin));
}

// ============================================================================
// UTF-8 helpers (Go utf8.Valid / AppendRune).
// ============================================================================

// Go utf8.Valid.
bool valid_utf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const DecodedRune d = decode_rune(s, i);
    if (d.r == 0xFFFD && d.size == 1) {
      return false;
    }
    i += static_cast<size_t>(d.size);
  }
  return true;
}

// UTF-8 encode one code point (Go utf8.AppendRune).
void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
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

// ============================================================================
// Legacy codepage tables — generated from golang.org/x/text/encoding/charmap
// (the exact decoder cliamp's sanitizeTag uses), one 128-entry table per
// byte 0x80-0xFF plus a per-byte "letter above U+024F" flag computed with Go
// unicode.IsLetter semantics (score in Go sanitizeTag).
// ============================================================================
static constexpr std::array<char32_t, 128> kCp1255 = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0xFFFD, 0x2039, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0xFFFD, 0x203A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x20AA, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00D7, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00F7, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x05B0, 0x05B1, 0x05B2, 0x05B3, 0x05B4, 0x05B5, 0x05B6, 0x05B7, 0x05B8, 0x05B9, 0x05BA, 0x05BB, 0x05BC, 0x05BD, 0x05BE, 0x05BF, 0x05C0, 0x05C1, 0x05C2, 0x05C3, 0x05F0, 0x05F1, 0x05F2, 0x05F3, 0x05F4, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF, 0x05E0, 0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x05E5, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, 0xFFFD, 0xFFFD, 0x200E, 0x200F, 0xFFFD};
static constexpr std::array<bool, 128> kCp1255_letters = {false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, true, true, false, false, false, false, false, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, false, false, false};

static constexpr std::array<char32_t, 128> kCp1256 = {0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688, 0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA, 0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061F, 0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627, 0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F, 0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7, 0x0637, 0x0638, 0x0639, 0x063A, 0x0640, 0x0641, 0x0642, 0x0643, 0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647, 0x0648, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064A, 0x00EE, 0x00EF, 0x064B, 0x064C, 0x064D, 0x064E, 0x00F4, 0x064F, 0x0650, 0x00F7, 0x0651, 0x00F9, 0x0652, 0x00FB, 0x00FC, 0x200E, 0x200F, 0x06D2};
static constexpr std::array<bool, 128> kCp1256_letters = {false, true, false, false, false, false, false, false, true, false, true, false, false, true, true, true, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, true, true, true, true, true, true, false, true, false, true, true, true, true, false, false, false, false, false, true, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true};

static constexpr std::array<char32_t, 128> kCp1251 = {0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F};
static constexpr std::array<bool, 128> kCp1251_letters = {true, true, false, true, false, false, false, false, false, false, true, false, true, true, true, true, true, false, false, false, false, false, false, false, false, false, true, false, true, true, true, true, false, true, true, true, false, true, false, false, true, false, true, false, false, false, false, true, false, false, true, true, true, false, false, false, true, false, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};

static constexpr std::array<char32_t, 128> kCp1253 = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0xFFFD, 0x2030, 0xFFFD, 0x2039, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0xFFFD, 0x203A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x00A0, 0x0385, 0x0386, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0xFFFD, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x2015, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x00B5, 0x00B6, 0x00B7, 0x0388, 0x0389, 0x038A, 0x00BB, 0x038C, 0x00BD, 0x038E, 0x038F, 0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0, 0x03A1, 0xFFFD, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x03AA, 0x03AB, 0x03AC, 0x03AD, 0x03AE, 0x03AF, 0x03B0, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0, 0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE, 0xFFFD};
static constexpr std::array<bool, 128> kCp1253_letters = {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, true, true, false, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false};

static constexpr std::array<char32_t, 128> kCp874 = {0x20AC, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2026, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x00A0, 0x0E01, 0x0E02, 0x0E03, 0x0E04, 0x0E05, 0x0E06, 0x0E07, 0x0E08, 0x0E09, 0x0E0A, 0x0E0B, 0x0E0C, 0x0E0D, 0x0E0E, 0x0E0F, 0x0E10, 0x0E11, 0x0E12, 0x0E13, 0x0E14, 0x0E15, 0x0E16, 0x0E17, 0x0E18, 0x0E19, 0x0E1A, 0x0E1B, 0x0E1C, 0x0E1D, 0x0E1E, 0x0E1F, 0x0E20, 0x0E21, 0x0E22, 0x0E23, 0x0E24, 0x0E25, 0x0E26, 0x0E27, 0x0E28, 0x0E29, 0x0E2A, 0x0E2B, 0x0E2C, 0x0E2D, 0x0E2E, 0x0E2F, 0x0E30, 0x0E31, 0x0E32, 0x0E33, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E38, 0x0E39, 0x0E3A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x0E3F, 0x0E40, 0x0E41, 0x0E42, 0x0E43, 0x0E44, 0x0E45, 0x0E46, 0x0E47, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E4D, 0x0E4E, 0x0E4F, 0x0E50, 0x0E51, 0x0E52, 0x0E53, 0x0E54, 0x0E55, 0x0E56, 0x0E57, 0x0E58, 0x0E59, 0x0E5A, 0x0E5B, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD};
static constexpr std::array<bool, 128> kCp874_letters = {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, false, false, false, false, false, false, false, false, false, false, false, false, true, true, true, true, true, true, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false};

// Go legacyEncodings order: Windows1255, Windows1256, Windows1251, Windows1253,
// Windows874. First best-scoring decode wins.
struct CodePage {
  const std::array<char32_t, 128>& table;
  const std::array<bool, 128>&     letters;
};
static constexpr CodePage kLegacyEncodings[] = {
    {kCp1255, kCp1255_letters},
    {kCp1256, kCp1256_letters},
    {kCp1251, kCp1251_letters},
    {kCp1253, kCp1253_letters},
    {kCp874, kCp874_letters},
};

// Go sanitizeTag: detect mojibake from legacy codepages and re-decode to UTF-8.
std::string sanitize_tag(std::string_view s) {
  if (s.empty()) {
    return std::string(s);
  }

  // Count runes in the Latin-1 supplement range (U+0080-U+00FF).
  int total = 0;
  int high_count = 0;
  for (size_t i = 0; i < s.size();) {
    const DecodedRune d = decode_rune(s, i);
    ++total;
    if (d.r >= 0x80 && d.r <= 0xFF) {
      ++high_count;
    }
    i += static_cast<size_t>(d.size);
  }
  if (total == 0 || high_count * 3 < total) {
    return std::string(s);
  }

  // Reverse the Latin-1 decode to recover the original tag bytes.
  std::string raw;
  raw.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const DecodedRune d = decode_rune(s, i);
    if (d.r > 0xFF) {
      return std::string(s);  // runes outside Latin-1 — not simple mojibake
    }
    raw.push_back(static_cast<char>(d.r));
    i += static_cast<size_t>(d.size);
  }

  // The original bytes might be valid UTF-8 that was double-decoded.
  if (valid_utf8(raw)) {
    return raw;
  }

  // Try legacy codepages; pick the one producing the most non-Latin letters
  // (Hebrew, Cyrillic, Arabic, Greek, Thai, etc.).
  std::string best_text;
  int best_score = 0;
  for (const CodePage& cp : kLegacyEncodings) {
    std::u32string text;
    text.reserve(raw.size());
    bool ok = true;
    int score = 0;
    for (const unsigned char b : raw) {
      const char32_t c = b < 0x80 ? static_cast<char32_t>(b) : cp.table[b - 0x80];
      if (c >= 0xD800 && c <= 0xDFFF) {
        ok = false;  // not valid UTF-8 (Go utf8.ValidString)
        break;
      }
      text.push_back(c);
      if (b >= 0x80 && cp.letters[b - 0x80]) {
        ++score;  // unicode.IsLetter(r) && r > 0x024F
      }
    }
    if (!ok) {
      continue;
    }
    if (score > best_score) {
      best_score = score;
      best_text.clear();
      for (const char32_t c : text) {
        append_utf8(best_text, c);
      }
    }
  }

  if (!best_text.empty()) {
    return best_text;
  }
  return std::string(s);
}

// ============================================================================
// Path helpers — Go path/filepath package semantics (slash, Linux).
// ============================================================================

// Go path.Base: last element of a slash-separated path.
std::string_view path_base(std::string_view p) {
  std::string_view q = p;
  while (q.size() > 1 && q.back() == '/') {
    q.remove_suffix(1);
  }
  if (q.empty()) {
    return ".";
  }
  if (q.back() == '/') {
    return "/";  // all slashes
  }
  const size_t slash = q.rfind('/');
  if (slash == std::string_view::npos) {
    return q;
  }
  return q.substr(slash + 1);
}

// Go path.Ext / filepath.Ext: suffix from the final dot of the last element.
std::string_view path_ext(std::string_view p) {
  for (size_t i = p.size(); i > 0; --i) {
    if (p[i - 1] == '/') {
      break;
    }
    if (p[i - 1] == '.') {
      return p.substr(i - 1);
    }
  }
  return "";
}

// Case-insensitive extension equality (Go strings.ToLower(ext) == want).
bool ext_is(std::string_view ext, std::string_view want) {
  if (ext.size() != want.size()) {
    return false;
  }
  for (size_t i = 0; i < ext.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(ext[i])) !=
        static_cast<unsigned char>(want[i])) {
      return false;
    }
  }
  return true;
}

// ============================================================================
// URL parsing — minimal Go url.Parse subset (scheme://host/path). Opaque URLs
// without "://" (e.g. "ytsearch:...") parse as invalid here; callers route
// them through the prefix/IsYTSearch checks exactly like Go's classifiers do.
// ============================================================================
struct url_parts {
  std::string_view host;
  std::string_view path;
  bool             ok;
};

url_parts parse_url(std::string_view s) {
  const size_t scheme_end = s.find("://");
  if (scheme_end == std::string_view::npos) {
    return {{}, {}, false};
  }
  std::string_view rest = s.substr(scheme_end + 3);
  const size_t q = rest.find_first_of("?#");
  if (q != std::string_view::npos) {
    rest = rest.substr(0, q);
  }
  const size_t path_pos = rest.find('/');
  std::string_view host = path_pos == std::string_view::npos ? rest : rest.substr(0, path_pos);
  std::string_view path = path_pos == std::string_view::npos ? std::string_view{} : rest.substr(path_pos);
  // Strip userinfo (Go url.Hostname strips it).
  const size_t at = host.rfind('@');
  if (at != std::string_view::npos) {
    host = host.substr(at + 1);
  }
  // Strip port (Go url.Hostname excludes it).
  const size_t colon = host.find(':');
  if (colon != std::string_view::npos) {
    host = host.substr(0, colon);
  }
  if (host.empty()) {
    return {{}, {}, false};
  }
  return {host, path, true};
}

// ============================================================================
// URL classifiers and Track-from-path construction (Go playlist.go).
// ============================================================================
bool match_search_prefix(std::string_view path, std::string_view name) {
  if (!starts_with(path, name)) {
    return false;
  }
  const std::string_view rest = path.substr(name.size());
  const size_t colon = rest.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  for (size_t i = 0; i < colon; ++i) {
    const char c = rest[i];
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// Go TrackFromFilename: "Artist - Title" parsing from the filename.
Track track_from_filename(std::string_view path) {
  const std::string_view base = path_base(path);
  const std::string_view ext = path_ext(base);
  const std::string_view name = ext.empty() ? base : base.substr(0, base.size() - ext.size());
  const std::string clean = sanitize_tag(name);
  Track t;
  t.path = std::string(path);
  const size_t sep = clean.find(" - ");  // Go strings.SplitN(name, " - ", 2)
  if (sep != std::string::npos) {
    t.artist = trim_space(std::string_view(clean).substr(0, sep));
    t.title = trim_space(std::string_view(clean).substr(sep + 3));
  } else {
    t.title = clean;
  }
  return t;
}

// Go trackFromURL: clean display title from the URL path.
Track track_from_url(std::string_view raw_url) {
  Track t;
  t.path = std::string(raw_url);
  t.stream = true;

  const url_parts u = parse_url(raw_url);
  if (!u.ok) {
    t.title = std::string(raw_url);
    return t;
  }

  // Extract filename from URL path using slash semantics (Go path package).
  const std::string_view base = path_base(u.path);
  if (!base.empty() && base != "." && base != "/") {
    const std::string_view ext = path_ext(base);
    const std::string_view name = ext.empty() ? base : base.substr(0, base.size() - ext.size());
    if (!name.empty() && name != "stream" && name != "rest") {
      t.title = std::string(name);
      return t;
    }
  }

  // Fallback: use hostname (Go keeps its case).
  t.title = std::string(u.host);
  return t;
}

// Go readTags: embedded tags via read_tags (tags.hpp), falling back to
// TrackFromFilename when tag reading fails or the tags have no title.
Track track_from_local(std::string_view path) {
  const auto tags = read_tags(path);
  if (!tags) {
    return track_from_filename(path);
  }
  const TagInfo& m = *tags;
  Track t;
  t.path = std::string(path);
  t.embedded_lyrics = sanitize_tag(trim_space(m.lyrics));
  if (trim_space(m.title).empty()) {
    Track fallback = track_from_filename(path);
    fallback.embedded_lyrics = t.embedded_lyrics;
    fallback.album_art_url = t.album_art_url;
    return fallback;
  }
  t.title = sanitize_tag(trim_space(m.title));
  t.artist = sanitize_tag(trim_space(m.artist));
  t.album = sanitize_tag(trim_space(m.album));
  t.genre = sanitize_tag(trim_space(m.genre));
  t.year = m.year;
  t.track_number = m.track_number;
  t.duration_secs = m.duration_secs;
  return t;
}

// ============================================================================
// Track helpers (Go playlist.go).
// ============================================================================
Track clone_track(const Track& track) {
  Track cloned = track;
  return cloned;  // std::map copies deep by value
}

std::vector<Track> clone_tracks(const std::vector<Track>& tracks) {
  return tracks;  // std::vector<std::string>/std::map members deep-copy
}

// Go reflect.DeepEqual on Track.
bool equal_track(const Track& a, const Track& b) {
  return a.path == b.path && a.title == b.title && a.artist == b.artist &&
         a.album == b.album && a.genre == b.genre && a.year == b.year &&
         a.track_number == b.track_number && a.stream == b.stream &&
         a.realtime == b.realtime && a.feed == b.feed &&
         a.duration_secs == b.duration_secs && a.bookmark == b.bookmark &&
         a.unplayable == b.unplayable && a.dir_sourced == b.dir_sourced &&
         a.embedded_lyrics == b.embedded_lyrics &&
         a.album_art_url == b.album_art_url &&
         a.provider_meta == b.provider_meta;
}

bool equal_tracks(const std::vector<Track>& a, const std::vector<Track>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!equal_track(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

// Go slices.Equal.
bool equal_indices(const std::vector<int>& a, const std::vector<int>& b) {
  return a == b;
}

// Go windowBounds.
std::pair<int, int> window_bounds(int length, int start, int limit) {
  start = std::max(0, start);
  if (limit <= 0 || start >= length) {
    return {0, 0};
  }
  int end = length;
  if (limit < end - start) {
    end = start + limit;
  }
  return {start, end};
}

// ============================================================================
// Playlist internal state helpers (Go Playlist private methods). These take
// the relevant state by reference so they can be shared across member
// functions without expanding the public class interface.
// ============================================================================
bool is_playable(int idx, const std::vector<Track>& tracks) {
  return idx >= 0 && idx < static_cast<int>(tracks.size()) && !tracks[static_cast<size_t>(idx)].unplayable;
}

std::optional<std::pair<int, int>> first_playable_order_slot(
    int from, int to, const std::vector<int>& order, const std::vector<Track>& tracks) {
  for (int i = from; i < to && i < static_cast<int>(order.size()); ++i) {
    const int idx = order[static_cast<size_t>(i)];
    if (is_playable(idx, tracks)) {
      return std::pair{i, idx};
    }
  }
  return std::nullopt;
}

std::optional<std::pair<int, int>> last_playable_order_slot(
    int from, const std::vector<int>& order, const std::vector<Track>& tracks) {
  if (from >= static_cast<int>(order.size())) {
    from = static_cast<int>(order.size()) - 1;
  }
  for (int i = from; i >= 0; --i) {
    const int idx = order[static_cast<size_t>(i)];
    if (is_playable(idx, tracks)) {
      return std::pair{i, idx};
    }
  }
  return std::nullopt;
}

struct QueuedPick {
  int               idx;
  std::vector<int>  remaining;
};

// Go nextPlayableQueued: first playable queued track + queue after it.
std::optional<QueuedPick> next_playable_queued(const std::vector<int>& queue,
                                               const std::vector<Track>& tracks) {
  for (size_t i = 0; i < queue.size(); ++i) {
    const int idx = queue[i];
    if (is_playable(idx, tracks)) {
      return QueuedPick{idx, std::vector<int>(queue.begin() + static_cast<std::ptrdiff_t>(i) + 1, queue.end())};
    }
  }
  return std::nullopt;
}

// Go atShuffleWrap.
bool at_shuffle_wrap(RepeatMode repeat, bool shuffle, bool queue_empty,
                     int queued_idx, int pos, size_t order_len) {
  return repeat == RepeatMode::All && shuffle && queue_empty && queued_idx == -1 &&
         static_cast<size_t>(pos + 1) >= order_len;
}

// Go doShuffle: Fisher-Yates over the non-current track indices, keeping the
// current track at the front, resetting pos to 0.
void do_shuffle(std::vector<int>& order, int& pos, size_t n_tracks) {
  const int cur = order[static_cast<size_t>(pos)];
  std::vector<int> others;
  others.reserve(n_tracks - 1);
  for (size_t i = 0; i < n_tracks; ++i) {
    if (static_cast<int>(i) != cur) {
      others.push_back(static_cast<int>(i));
    }
  }
  for (size_t k = others.size(); k > 1; --k) {
    const size_t i = k - 1;
    const int j = rng_int(static_cast<int>(k));
    std::swap(others[i], others[static_cast<size_t>(j)]);
  }
  order.clear();
  order.reserve(n_tracks);
  order.push_back(cur);
  order.insert(order.end(), others.begin(), others.end());
  pos = 0;
}

// Go nextShuffleWrap: reshuffle and pick the next playable after the current;
// restores the original order/pos when nothing is playable.
std::optional<std::pair<int, int>> next_shuffle_wrap(std::vector<int>& order, int& pos,
                                                     const std::vector<Track>& tracks) {
  const std::vector<int> orig_order = order;
  const int orig_pos = pos;
  do_shuffle(order, pos, tracks.size());
  if (auto r = first_playable_order_slot(1, static_cast<int>(order.size()), order, tracks)) {
    return r;
  }
  if (auto r = first_playable_order_slot(0, 1, order, tracks)) {
    return r;
  }
  order = orig_order;
  pos = orig_pos;
  return std::nullopt;
}

// Go advanceFromOrder.
std::optional<std::pair<int, int>> advance_from_order(
    std::vector<int>& order, int& pos, const std::vector<Track>& tracks,
    bool shuffle, RepeatMode repeat, bool queue_empty, int queued_idx) {
  if (auto r = first_playable_order_slot(pos + 1, static_cast<int>(order.size()), order, tracks)) {
    return r;
  }
  if (repeat != RepeatMode::All) {
    return std::nullopt;
  }
  if (shuffle && at_shuffle_wrap(repeat, shuffle, queue_empty, queued_idx, pos, order.size())) {
    return next_shuffle_wrap(order, pos, tracks);
  }
  return first_playable_order_slot(0, static_cast<int>(order.size()), order, tracks);
}

// Go resolveSelectedPlayablePos.
std::optional<std::pair<int, int>> resolve_selected_playable_pos(
    const std::vector<int>& order, int pos, const std::vector<Track>& tracks,
    RepeatMode repeat) {
  if (order.empty()) {
    return std::nullopt;
  }
  const int idx = order[static_cast<size_t>(pos)];
  if (is_playable(idx, tracks)) {
    return std::pair{pos, idx};
  }
  if (auto r = first_playable_order_slot(pos + 1, static_cast<int>(order.size()), order, tracks)) {
    return r;
  }
  if (repeat == RepeatMode::All) {
    return first_playable_order_slot(0, pos, order, tracks);
  }
  return std::nullopt;
}

int current_order_track_index(const std::vector<int>& order, int pos) {
  if (order.empty()) {
    return -1;
  }
  return order[static_cast<size_t>(pos)];
}

int current_track_index(const std::vector<int>& order, int pos, int queued_idx) {
  if (order.empty()) {
    return -1;
  }
  if (queued_idx >= 0) {
    return queued_idx;
  }
  return current_order_track_index(order, pos);
}

// Go matches: deep state comparison against a prior Snapshot.
bool matches_state(const Snapshot& s, const std::vector<Track>& tracks,
                   const std::vector<int>& order, int pos, bool shuffle,
                   RepeatMode repeat, const std::vector<int>& queue,
                   int queued_idx) {
  return equal_tracks(tracks, s.tracks) && equal_indices(order, s.order) &&
         pos == s.pos && shuffle == s.shuffle && repeat == s.repeat &&
         equal_indices(queue, s.queue) && queued_idx == s.queued_idx;
}

}  // namespace

// ============================================================================
// Track member functions.
// ============================================================================
std::string Track::meta(std::string_view key) const {
  const auto it = provider_meta.find(std::string(key));
  return it == provider_meta.end() ? std::string{} : it->second;
}

std::string Track::display_name() const {
  if (!artist.empty()) {
    return artist + " - " + title;
  }
  return title;
}

bool Track::is_album() const {
  return meta(kMetaKind) == std::string(kMetaKindAlbum);
}

std::string Track::album_id() const {
  if (!is_album()) {
    return {};
  }
  return meta(kMetaAlbumID);
}

// ============================================================================
// Free functions (Go playlist.go).
// ============================================================================
std::string_view to_string(RepeatMode r) {
  switch (r) {
    case RepeatMode::All:
      return "All";
    case RepeatMode::One:
      return "One";
    default:
      return "Off";
  }
}

int total_duration_secs(const std::vector<Track>& tracks) {
  int total = 0;
  for (const Track& t : tracks) {
    if (t.duration_secs > 0) {
      total += t.duration_secs;
    }
  }
  return total;
}

// Go IsURL: HTTP/HTTPS prefix or yt-dlp search protocol string.
bool is_url(std::string_view path) {
  return starts_with(path, "http://") || starts_with(path, "https://") ||
         is_ytsearch(path);
}

// Go IsYTSearch: ytsearch:/ytsearchN:/scsearch:/scsearchN:.
bool is_ytsearch(std::string_view path) {
  return match_search_prefix(path, "ytsearch") || match_search_prefix(path, "scsearch");
}

// Go IsM3U.
bool is_m3u(std::string_view path) {
  if (is_url(path)) {
    const url_parts u = parse_url(path);
    if (!u.ok) {
      return false;
    }
    return ext_is(path_ext(u.path), ".m3u") || ext_is(path_ext(u.path), ".m3u8");
  }
  return ext_is(path_ext(path), ".m3u") || ext_is(path_ext(path), ".m3u8");
}

// Go IsLocalM3U.
bool is_local_m3u(std::string_view path) {
  return !is_url(path) && is_m3u(path);
}

// Go IsPLS.
bool is_pls(std::string_view path) {
  if (is_url(path)) {
    const url_parts u = parse_url(path);
    if (!u.ok) {
      return false;
    }
    return ext_is(path_ext(u.path), ".pls");
  }
  return ext_is(path_ext(path), ".pls");
}

// Go IsLocalPLS.
bool is_local_pls(std::string_view path) {
  return !is_url(path) && is_pls(path);
}

// Go IsYouTubeURL.
bool is_youtube_url(std::string_view path) {
  if (!is_url(path)) {
    return false;
  }
  if (is_ytsearch(path)) {
    return false;  // search protocols are handled by yt-dlp
  }
  const url_parts u = parse_url(path);
  if (!u.ok) {
    return false;
  }
  const std::string host = lower_ascii(trim_host_prefix(u.host));
  return host == "youtube.com" || host == "youtu.be";
}

// Go IsYouTubeMusicURL.
bool is_youtube_music_url(std::string_view path) {
  if (!is_url(path)) {
    return false;
  }
  const url_parts u = parse_url(path);
  if (!u.ok) {
    return false;
  }
  const std::string host = lower_ascii(trim_host_prefix(u.host));
  return host == "music.youtube.com";
}

// Go IsYTDL.
bool is_ytdl(std::string_view path) {
  if (!is_url(path)) {
    return false;
  }
  if (is_youtube_url(path) || is_youtube_music_url(path)) {
    return true;
  }
  if (is_ytsearch(path)) {
    return true;
  }
  const url_parts u = parse_url(path);
  if (!u.ok) {
    return false;
  }
  const std::string host = lower_ascii(trim_host_prefix(u.host));
  if (host == "soundcloud.com" || host == "bandcamp.com" ||
      host == "music.163.com" || host == "bilibili.com" || host == "b23.tv") {
    return true;
  }
  // Bilibili subdomains (e.g. space.bilibili.com).
  if (ends_with(host, ".bilibili.com")) {
    return true;
  }
  // Bandcamp artist subdomains (e.g. artist.bandcamp.com).
  if (ends_with(host, ".bandcamp.com")) {
    return true;
  }
  return false;
}

// Go IsXiaoyuzhouEpisode.
bool is_xiaoyuzhou_episode(std::string_view path) {
  if (!is_url(path)) {
    return false;
  }
  const url_parts u = parse_url(path);
  if (!u.ok) {
    return false;
  }
  const std::string host = lower_ascii(trim_host_prefix(u.host));
  if (host != "xiaoyuzhoufm.com") {
    return false;
  }
  constexpr std::string_view kEpisodePrefix = "/episode/";
  if (u.path.size() < kEpisodePrefix.size()) {
    return false;
  }
  for (size_t i = 0; i < kEpisodePrefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(u.path[i])) !=
        static_cast<unsigned char>(kEpisodePrefix[i])) {
      return false;
    }
  }
  return true;
}

// Go IsFeed.
bool is_feed(std::string_view path) {
  if (!is_url(path)) {
    return false;
  }
  const url_parts u = parse_url(path);
  if (!u.ok) {
    return false;
  }
  const std::string_view ext = path_ext(u.path);
  return ext_is(ext, ".xml") || ext_is(ext, ".rss") || ext_is(ext, ".atom");
}

// Go TrackFromPath.
Track track_from_path(std::string_view path) {
  if (is_url(path)) {
    return track_from_url(path);
  }
  return track_from_local(path);
}

// Go CycleRepeat helper: Off -> All -> One.
RepeatMode cycle_repeat(RepeatMode r) {
  return static_cast<RepeatMode>((static_cast<int>(r) + 1) % 3);
}

// ============================================================================
// Playlist.
// ============================================================================
void Playlist::replace(const std::vector<Track>& tracks) {
  std::lock_guard<std::mutex> lock(mu_);
  Snapshot before;
  before.tracks = clone_tracks(tracks_);
  before.order = order_;
  before.pos = pos_;
  before.shuffle = shuffle_;
  before.repeat = repeat_;
  before.queue = queue_;
  before.queued_idx = queued_idx_;

  tracks_ = clone_tracks(tracks);
  order_.resize(tracks_.size());
  for (size_t i = 0; i < order_.size(); ++i) {
    order_[i] = static_cast<int>(i);
  }
  pos_ = 0;
  queue_.clear();
  queue_positions_.clear();
  queued_idx_ = -1;
  rebuild_bookmark_count_locked();
  if (shuffle_ && !tracks_.empty()) {
    do_shuffle_locked();
  }
  if (!matches_state(before, tracks_, order_, pos_, shuffle_, repeat_, queue_, queued_idx_)) {
    ++revision_;
  }
}

void Playlist::add(const std::vector<Track>& tracks) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::vector<Track> cloned = clone_tracks(tracks);
  if (cloned.empty()) {
    return;
  }
  const size_t start = tracks_.size();
  tracks_.insert(tracks_.end(), cloned.begin(), cloned.end());
  for (const Track& t : cloned) {
    if (t.bookmark) {
      ++bookmark_count_;
    }
  }
  for (size_t i = start; i < tracks_.size(); ++i) {
    order_.push_back(static_cast<int>(i));
  }
  if (shuffle_) {
    // Shuffle mode: mix newly added tracks into the upcoming playback order
    // without disturbing already-played items or the current position.
    if (start == 0) {
      pos_ = 0;
      do_shuffle_locked();
    } else {
      if (pos_ < 0) {
        pos_ = 0;
      }
      if (static_cast<size_t>(pos_) >= order_.size()) {
        // Inconsistent internal state; recover by re-shuffling so newly added
        // tracks don't end up in sequential order.
        pos_ = 0;
        do_shuffle_locked();
      } else {
        // Fisher-Yates over the upcoming tail of order in place.
        const size_t tail_start = static_cast<size_t>(pos_) + 1;
        const size_t tail_len = order_.size() - tail_start;
        for (size_t k = tail_len; k > 1; --k) {
          const size_t i = k - 1;
          const int j = rng_int(static_cast<int>(k));
          std::swap(order_[tail_start + i], order_[tail_start + static_cast<size_t>(j)]);
        }
      }
    }
  }
  ++revision_;
}

int Playlist::len() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<int>(tracks_.size());
}

std::pair<Track, int> Playlist::current() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (tracks_.empty()) {
    return {Track{}, -1};
  }
  const int idx = current_track_index(order_, pos_, queued_idx_);
  return {clone_track(tracks_[static_cast<size_t>(idx)]), idx};
}

int Playlist::index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_track_index(order_, pos_, queued_idx_);
}

bool Playlist::current_is_queued() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queued_idx_ >= 0;
}

std::optional<SelectionActivation> Playlist::activate_selected() {
  std::lock_guard<std::mutex> lock(mu_);
  const int orig_pos = pos_;
  const int orig_queued_idx = queued_idx_;
  const int selected_pos = pos_;
  const auto res = resolve_selected_playable_pos(order_, pos_, tracks_, repeat_);
  if (!res) {
    return std::nullopt;
  }
  pos_ = res->first;
  queued_idx_ = -1;
  if (pos_ != orig_pos || queued_idx_ != orig_queued_idx) {
    ++revision_;
  }
  SelectionActivation act;
  act.track = clone_track(tracks_[static_cast<size_t>(res->second)]);
  act.index = res->second;
  act.skipped = res->first != selected_pos;
  return act;
}

std::pair<Track, bool> Playlist::next() {
  std::lock_guard<std::mutex> lock(mu_);
  if (tracks_.empty()) {
    return {Track{}, false};
  }
  const int orig_pos = pos_;
  const int orig_queued_idx = queued_idx_;

  if (const auto q = next_playable_queued(queue_, tracks_)) {
    queue_ = q->remaining;
    rebuild_queue_positions_locked();
    queued_idx_ = q->idx;
    ++revision_;
    return {clone_track(tracks_[static_cast<size_t>(q->idx)]), true};
  }
  bool queue_cleared = false;
  if (!queue_.empty()) {
    queue_.clear();
    queue_positions_.clear();
    queue_cleared = true;
  }
  if (repeat_ == RepeatMode::One) {
    const int idx = current_order_track_index(order_, pos_);
    if (is_playable(idx, tracks_)) {
      queued_idx_ = -1;
      if (queue_cleared || orig_queued_idx != -1) {
        ++revision_;
      }
      return {clone_track(tracks_[static_cast<size_t>(idx)]), true};
    }
    pos_ = orig_pos;
    queued_idx_ = orig_queued_idx;
    if (queue_cleared) {
      ++revision_;
    }
    return {Track{}, false};
  }

  const bool shuffle_wrap =
      at_shuffle_wrap(repeat_, shuffle_, queue_.empty(), queued_idx_, pos_, order_.size());
  std::vector<int> orig_order;
  if (shuffle_wrap) {
    orig_order = order_;
  }
  const auto adv = advance_from_order(order_, pos_, tracks_, shuffle_, repeat_,
                                      queue_.empty(), queued_idx_);
  if (!adv) {
    pos_ = orig_pos;
    queued_idx_ = orig_queued_idx;
    if (queue_cleared) {
      ++revision_;
    }
    return {Track{}, false};
  }
  queued_idx_ = -1;
  pos_ = adv->first;
  if (queue_cleared || orig_queued_idx != -1 || orig_pos != adv->first ||
      (shuffle_wrap && order_ != orig_order)) {
    ++revision_;
  }
  return {clone_track(tracks_[static_cast<size_t>(adv->second)]), true};
}

std::pair<Track, bool> Playlist::peek_next() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (tracks_.empty()) {
    return {Track{}, false};
  }
  if (const auto q = next_playable_queued(queue_, tracks_)) {
    return {clone_track(tracks_[static_cast<size_t>(q->idx)]), true};
  }
  if (repeat_ == RepeatMode::One) {
    const int idx = current_order_track_index(order_, pos_);
    if (is_playable(idx, tracks_)) {
      return {clone_track(tracks_[static_cast<size_t>(idx)]), true};
    }
    return {Track{}, false};
  }
  if (at_shuffle_wrap(repeat_, shuffle_, queue_.empty(), queued_idx_, pos_, order_.size())) {
    return {Track{}, false};  // next can't be predicted (shuffle wrap)
  }
  // advance_from_order mutates order/pos only on the shuffle-wrap path, which
  // is excluded above; pass copies to keep peek_next const.
  std::vector<int> order = order_;
  int pos = pos_;
  const auto adv = advance_from_order(order, pos, tracks_, shuffle_, repeat_,
                                      queue_.empty(), queued_idx_);
  if (!adv) {
    return {Track{}, false};
  }
  return {clone_track(tracks_[static_cast<size_t>(adv->second)]), true};
}

std::pair<Track, bool> Playlist::prev() {
  std::lock_guard<std::mutex> lock(mu_);
  if (tracks_.empty()) {
    return {Track{}, false};
  }
  const int orig_pos = pos_;
  const int orig_queued_idx = queued_idx_;
  queued_idx_ = -1;

  if (const auto lp = last_playable_order_slot(pos_ - 1, order_, tracks_)) {
    pos_ = lp->first;
    if (pos_ != orig_pos || queued_idx_ != orig_queued_idx) {
      ++revision_;
    }
    return {clone_track(tracks_[static_cast<size_t>(lp->second)]), true};
  }
  if (repeat_ == RepeatMode::All) {
    if (const auto lp = last_playable_order_slot(static_cast<int>(order_.size()) - 1, order_, tracks_)) {
      pos_ = lp->first;
      if (pos_ != orig_pos || queued_idx_ != orig_queued_idx) {
        ++revision_;
      }
      return {clone_track(tracks_[static_cast<size_t>(lp->second)]), true};
    }
  }
  pos_ = orig_pos;
  queued_idx_ = orig_queued_idx;
  if (orig_queued_idx >= 0) {
    return {clone_track(tracks_[static_cast<size_t>(orig_queued_idx)]), false};
  }
  return {clone_track(tracks_[static_cast<size_t>(order_[static_cast<size_t>(pos_)])]), false};
}

void Playlist::set_index(int i) {
  std::lock_guard<std::mutex> lock(mu_);
  const int orig_pos = pos_;
  const int orig_queued_idx = queued_idx_;
  queued_idx_ = -1;
  for (size_t pos = 0; pos < order_.size(); ++pos) {
    if (order_[pos] == i) {
      pos_ = static_cast<int>(pos);
      break;
    }
  }
  if (pos_ != orig_pos || queued_idx_ != orig_queued_idx) {
    ++revision_;
  }
}

void Playlist::queue(int track_idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (track_idx < 0 || track_idx >= static_cast<int>(tracks_.size())) {
    return;
  }
  queue_.push_back(track_idx);
  if (queue_positions_.find(track_idx) == queue_positions_.end()) {
    queue_positions_[track_idx] = static_cast<int>(queue_.size());
  }
  ++revision_;
}

bool Playlist::dequeue(int track_idx) {
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (queue_[i] == track_idx) {
      queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
      rebuild_queue_positions_locked();
      ++revision_;
      return true;
    }
  }
  return false;
}

int Playlist::queue_position(int track_idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = queue_positions_.find(track_idx);
  return it == queue_positions_.end() ? 0 : it->second;
}

int Playlist::queue_len() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<int>(queue_.size());
}

std::vector<Track> Playlist::queue_tracks() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Track> tracks;
  tracks.reserve(queue_.size());
  for (const int idx : queue_) {
    tracks.push_back(clone_track(tracks_[static_cast<size_t>(idx)]));
  }
  return tracks;
}

std::vector<QueueEntry> Playlist::queue_entries() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<QueueEntry> entries;
  entries.reserve(queue_.size());
  for (size_t i = 0; i < queue_.size(); ++i) {
    const int index = queue_[i];
    entries.push_back(QueueEntry{index, clone_track(tracks_[static_cast<size_t>(index)])});
  }
  return entries;
}

std::vector<Track> Playlist::queue_window(int start, int limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto [begin, end] = window_bounds(static_cast<int>(queue_.size()), start, limit);
  if (begin == end) {
    return {};
  }
  std::vector<Track> tracks;
  tracks.reserve(static_cast<size_t>(end - begin));
  for (int i = begin; i < end; ++i) {
    tracks.push_back(clone_track(tracks_[static_cast<size_t>(queue_[static_cast<size_t>(i)])]));
  }
  return tracks;
}

void Playlist::clear_queue() {
  std::lock_guard<std::mutex> lock(mu_);
  if (queue_.empty() && queue_positions_.empty()) {
    return;
  }
  queue_.clear();
  queue_positions_.clear();
  ++revision_;
}

void Playlist::remove_queue_at(int pos) {
  std::lock_guard<std::mutex> lock(mu_);
  if (pos >= 0 && pos < static_cast<int>(queue_.size())) {
    queue_.erase(queue_.begin() + pos);
    rebuild_queue_positions_locked();
    ++revision_;
  }
}

bool Playlist::move_queue(int from, int to) {
  std::lock_guard<std::mutex> lock(mu_);
  if (from < 0 || from >= static_cast<int>(queue_.size()) ||
      to < 0 || to >= static_cast<int>(queue_.size()) || from == to) {
    return false;
  }
  std::swap(queue_[static_cast<size_t>(from)], queue_[static_cast<size_t>(to)]);
  rebuild_queue_positions_locked();
  ++revision_;
  return true;
}

bool Playlist::move(int from, int to) {
  std::lock_guard<std::mutex> lock(mu_);
  if (from < 0 || from >= static_cast<int>(tracks_.size()) ||
      to < 0 || to >= static_cast<int>(tracks_.size()) || from == to) {
    return false;
  }

  // Swap in the tracks array (visual order).
  std::swap(tracks_[static_cast<size_t>(from)], tracks_[static_cast<size_t>(to)]);

  // Update order: swap all references so they point at the moved tracks.
  for (int& idx : order_) {
    if (idx == from) {
      idx = to;
    } else if (idx == to) {
      idx = from;
    }
  }

  // Queue also references track indices.
  for (int& idx : queue_) {
    if (idx == from) {
      idx = to;
    } else if (idx == to) {
      idx = from;
    }
  }
  rebuild_queue_positions_locked();
  if (queued_idx_ == from) {
    queued_idx_ = to;
  } else if (queued_idx_ == to) {
    queued_idx_ = from;
  }

  // When shuffle is off, reset order to [0,1,...,n] so playback follows
  // the new visual order rather than preserving the old sequence.
  if (!shuffle_) {
    const int cur = order_[static_cast<size_t>(pos_)];
    for (size_t i = 0; i < order_.size(); ++i) {
      order_[i] = static_cast<int>(i);
    }
    pos_ = cur;
  }

  ++revision_;
  return true;
}

bool Playlist::remove(int idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < 0 || idx >= static_cast<int>(tracks_.size())) {
    return false;
  }
  if (tracks_[static_cast<size_t>(idx)].bookmark) {
    --bookmark_count_;
  }

  tracks_.erase(tracks_.begin() + idx);

  int removed_order_pos = -1;
  std::vector<int> new_order;
  new_order.reserve(order_.size());
  for (size_t i = 0; i < order_.size(); ++i) {
    int ord = order_[i];
    if (ord == idx) {
      removed_order_pos = static_cast<int>(i);
      continue;
    }
    if (ord > idx) {
      --ord;
    }
    new_order.push_back(ord);
  }
  order_ = std::move(new_order);

  if (removed_order_pos >= 0 && removed_order_pos < pos_) {
    --pos_;
  }
  if (pos_ >= static_cast<int>(order_.size())) {
    pos_ = static_cast<int>(order_.size()) - 1;
  }
  if (pos_ < 0) {
    pos_ = 0;
  }

  std::vector<int> new_queue;
  new_queue.reserve(queue_.size());
  for (const int q : queue_) {
    if (q == idx) {
      continue;
    }
    if (q > idx) {
      new_queue.push_back(q - 1);
    } else {
      new_queue.push_back(q);
    }
  }
  queue_ = std::move(new_queue);
  rebuild_queue_positions_locked();

  if (queued_idx_ == idx) {
    queued_idx_ = -1;
  } else if (queued_idx_ > idx) {
    --queued_idx_;
  }

  ++revision_;
  return true;
}

void Playlist::set_track(int i, const Track& t) {
  std::lock_guard<std::mutex> lock(mu_);
  if (i < 0 || i >= static_cast<int>(tracks_.size())) {
    return;
  }
  Track cloned = clone_track(t);
  if (equal_track(tracks_[static_cast<size_t>(i)], cloned)) {
    return;
  }
  if (tracks_[static_cast<size_t>(i)].bookmark != cloned.bookmark) {
    if (cloned.bookmark) {
      ++bookmark_count_;
    } else {
      --bookmark_count_;
    }
  }
  tracks_[static_cast<size_t>(i)] = std::move(cloned);
  ++revision_;
}

std::vector<Track> Playlist::tracks() const {
  std::lock_guard<std::mutex> lock(mu_);
  return clone_tracks(tracks_);
}

std::optional<Track> Playlist::track(int index) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (index < 0 || index >= static_cast<int>(tracks_.size())) {
    return std::nullopt;
  }
  return clone_track(tracks_[static_cast<size_t>(index)]);
}

std::vector<Track> Playlist::track_window(int start, int limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto [begin, end] = window_bounds(static_cast<int>(tracks_.size()), start, limit);
  if (begin == end) {
    return {};
  }
  return std::vector<Track>(tracks_.begin() + begin, tracks_.begin() + end);
}

void Playlist::toggle_bookmark(int idx) {
  std::lock_guard<std::mutex> lock(mu_);
  if (idx >= 0 && idx < static_cast<int>(tracks_.size())) {
    tracks_[static_cast<size_t>(idx)].bookmark = !tracks_[static_cast<size_t>(idx)].bookmark;
    if (tracks_[static_cast<size_t>(idx)].bookmark) {
      ++bookmark_count_;
    } else {
      --bookmark_count_;
    }
    ++revision_;
  }
}

int Playlist::bookmark_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return bookmark_count_;
}

void Playlist::toggle_shuffle() {
  std::lock_guard<std::mutex> lock(mu_);
  shuffle_ = !shuffle_;
  if (tracks_.empty()) {
    ++revision_;
    return;
  }
  if (shuffle_) {
    do_shuffle_locked();
  } else {
    const int cur = order_[static_cast<size_t>(pos_)];
    order_.resize(tracks_.size());
    for (size_t i = 0; i < order_.size(); ++i) {
      order_[i] = static_cast<int>(i);
    }
    pos_ = cur;
  }
  ++revision_;
}

bool Playlist::shuffled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return shuffle_;
}

void Playlist::cycle_repeat() {
  std::lock_guard<std::mutex> lock(mu_);
  repeat_ = static_cast<RepeatMode>((static_cast<int>(repeat_) + 1) % 3);
  ++revision_;
}

void Playlist::set_repeat(RepeatMode mode) {
  std::lock_guard<std::mutex> lock(mu_);
  if (repeat_ != mode) {
    repeat_ = mode;
    ++revision_;
  }
}

RepeatMode Playlist::repeat() const {
  std::lock_guard<std::mutex> lock(mu_);
  return repeat_;
}

Snapshot Playlist::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  Snapshot s;
  s.tracks = clone_tracks(tracks_);
  s.order = order_;
  s.pos = pos_;
  s.shuffle = shuffle_;
  s.repeat = repeat_;
  s.queue = queue_;
  s.queued_idx = queued_idx_;
  return s;
}

void Playlist::restore(const Snapshot& s) {
  std::lock_guard<std::mutex> lock(mu_);
  Snapshot before;
  before.tracks = clone_tracks(tracks_);
  before.order = order_;
  before.pos = pos_;
  before.shuffle = shuffle_;
  before.repeat = repeat_;
  before.queue = queue_;
  before.queued_idx = queued_idx_;

  tracks_ = clone_tracks(s.tracks);
  order_ = s.order;
  pos_ = s.pos;
  shuffle_ = s.shuffle;
  repeat_ = s.repeat;
  queue_ = s.queue;
  queued_idx_ = s.queued_idx;
  rebuild_queue_positions_locked();
  rebuild_bookmark_count_locked();
  if (!matches_state(before, tracks_, order_, pos_, shuffle_, repeat_, queue_, queued_idx_)) {
    ++revision_;
  }
}

// ============================================================================
// Private locked helpers.
// ============================================================================
void Playlist::do_shuffle_locked() {
  do_shuffle(order_, pos_, tracks_.size());
}

void Playlist::rebuild_queue_positions_locked() {
  queue_positions_.clear();
  if (queue_.empty()) {
    return;
  }
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (queue_positions_.find(queue_[i]) == queue_positions_.end()) {
      queue_positions_[queue_[i]] = static_cast<int>(i) + 1;
    }
  }
}

void Playlist::rebuild_bookmark_count_locked() {
  bookmark_count_ = 0;
  for (const Track& t : tracks_) {
    if (t.bookmark) {
      ++bookmark_count_;
    }
  }
}

}  // namespace bootamp::playlist
