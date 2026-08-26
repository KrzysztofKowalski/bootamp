// playlist/tags.cpp — TagLib wrapper for reading embedded tags.
//
// Port of cliamp/playlist/tags.go (readTagsWithOptions + cacheAlbumArt +
// sanitizeTag). read_tags() is the public entry point: it parses ID3v2/ID3v1
// (MP3, DSF), Vorbis comments (FLAC, Ogg Vorbis/Opus/Speex) and MP4 atoms,
// sanitizes mojibake from legacy codepages, extracts embedded lyrics, caches
// embedded album art under <data_dir()>/album-art (sha256-named, file://
// URL) and fills duration from the audio properties.
//
// Divergence from Go: cliamp's readTags (TrackFromPath path) never caches
// album art — only RefreshEmbeddedMetadata does (readTagsWithOptions(path,
// true)). bootamp has a single read_tags() and no separate art-extraction
// API, so art is always cached here (equivalent to Go's cacheArt=true).
// Also fills duration_secs (Go leaves it 0). TagLib reads a wider format
// set (WAV/AIFF/WavPack/APE/ASF get text tags where Go's tag.ReadFrom
// returns ErrNoTagsFound); filename fallback lives in the caller.
#include "playlist/tags.hpp"

#include "foundation/appdir.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/audioproperties.h>
#include <taglib/dsffile.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2frame.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/speexfile.h>
#include <taglib/tag.h>
#include <taglib/tbytevector.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bootamp::playlist {

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Go names art-cache files by hex(sha256(picture data));
// bootamp carries a compact self-contained implementation instead of pulling
// in a crypto dependency.
// ---------------------------------------------------------------------------

inline constexpr std::array<std::uint32_t, 64> kSha256K = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline constexpr std::array<std::uint32_t, 8> kSha256Init = {
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

std::array<std::uint32_t, 8> sha256_compress(std::array<std::uint32_t, 8> h,
                                             const unsigned char* block) {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + maj;
    hh = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  return h;
}

// sha256_hex returns the lowercase hex digest of `data[0..len)`.
std::string sha256_hex(const char* data, std::size_t len) {
  std::array<std::uint32_t, 8> h = kSha256Init;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  std::size_t pos = 0;
  while (pos + 64 <= len) {
    h = sha256_compress(h, bytes + pos);
    pos += 64;
  }
  // Final block(s): remaining bytes + 0x80 pad + 64-bit big-endian bit
  // length. When the pad byte lands at offset >= 56 the length no longer
  // fits in the first block and a second all-zero block carries it.
  std::array<unsigned char, 128> tail{};
  const std::size_t rem = len - pos;
  std::memcpy(tail.data(), bytes + pos, rem);
  tail[rem] = 0x80;
  const bool two_blocks = rem + 1 > 56;
  const std::size_t len_off = two_blocks ? 120 : 56;
  const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
  for (std::size_t i = 0; i < 8; ++i) {
    tail[len_off + i] = static_cast<unsigned char>((bits >> (56 - i * 8)) & 0xFF);
  }
  h = sha256_compress(h, tail.data());
  if (two_blocks) {
    h = sha256_compress(h, tail.data() + 64);
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint32_t v : h) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(v >> shift) & 0xF]);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// UTF-8 helpers (Go range / utf8 package semantics: invalid or truncated
// sequences decode as U+FFFD, one per byte).
// ---------------------------------------------------------------------------

std::vector<char32_t> utf8_decode(std::string_view s) {
  std::vector<char32_t> runes;
  runes.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
      runes.push_back(b0);
      ++i;
      continue;
    }
    int need = 0;
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; cp = b0 & 0x1F; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { need = 2; cp = b0 & 0x0F; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { need = 3; cp = b0 & 0x07; }
    if (need == 0 || i + static_cast<std::size_t>(need) + 1 > s.size()) {
      runes.push_back(0xFFFD);  // invalid lead or truncated sequence
      ++i;
      continue;
    }
    bool ok = true;
    for (int k = 1; k <= need; ++k) {
      const auto bk = static_cast<unsigned char>(s[i + k]);
      if ((bk & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (bk & 0x3F);
    }
    if (!ok || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      runes.push_back(0xFFFD);
      ++i;
      continue;
    }
    runes.push_back(cp);
    i += static_cast<std::size_t>(need) + 1;
  }
  return runes;
}

std::string utf8_encode(const std::vector<char32_t>& runes) {
  std::string out;
  for (const char32_t cp : runes) {
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
  return out;
}

bool is_valid_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { ++i; continue; }
    int need = 0;
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; cp = b0 & 0x1F; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { need = 2; cp = b0 & 0x0F; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { need = 3; cp = b0 & 0x07; }
    else { return false; }
    if (i + static_cast<std::size_t>(need) + 1 > s.size()) return false;
    for (int k = 1; k <= need; ++k) {
      const auto bk = static_cast<unsigned char>(s[i + k]);
      if ((bk & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (bk & 0x3F);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      return false;
    }
    i += static_cast<std::size_t>(need) + 1;
  }
  return true;
}

// trim_ascii matches Go's strings.TrimSpace for ASCII whitespace (the
// characters tag fields realistically contain).
std::string trim_ascii(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e) {
    const char c = s[b];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') break;
    ++b;
  }
  while (e > b) {
    const char c = s[e - 1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') break;
    --e;
  }
  return std::string(s.substr(b, e - b));
}

// ---------------------------------------------------------------------------
// Legacy codepage tables (cliamp playlist/encoding.go legacyEncodings, in the
// same order Go tries them): Windows-1255 (Hebrew), 1256 (Arabic), 1251
// (Cyrillic), 1253 (Greek), 874 (Thai). Byte 0x80-0xFF -> codepoint; 0 means
// undefined (Go's charmap decodes those as U+FFFD). Generated from the
// golang.org/x/text charmap tables.
// ---------------------------------------------------------------------------

// clang-format off
inline constexpr std::array<std::uint16_t, 128> kCP1255 = {
  8364, 0, 8218, 402, 8222, 8230, 8224, 8225, 710, 8240, 0, 8249, 0, 0, 0, 0,
  0, 8216, 8217, 8220, 8221, 8226, 8211, 8212, 732, 8482, 0, 8250, 0, 0, 0, 0,
  160, 161, 162, 163, 8362, 165, 166, 167, 168, 169, 215, 171, 172, 173, 174, 175,
  176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 247, 187, 188, 189, 190, 191,
  1456, 1457, 1458, 1459, 1460, 1461, 1462, 1463, 1464, 1465, 0, 1467, 1468, 1469, 1470, 1471,
  1472, 1473, 1474, 1475, 1520, 1521, 1522, 1523, 1524, 0, 0, 0, 0, 0, 0, 0,
  1488, 1489, 1490, 1491, 1492, 1493, 1494, 1495, 1496, 1497, 1498, 1499, 1500, 1501, 1502, 1503,
  1504, 1505, 1506, 1507, 1508, 1509, 1510, 1511, 1512, 1513, 1514, 0, 0, 8206, 8207, 0,
};
inline constexpr std::array<std::uint16_t, 128> kCP1256 = {
  8364, 1662, 8218, 402, 8222, 8230, 8224, 8225, 710, 8240, 1657, 8249, 338, 1670, 1688, 1672,
  1711, 8216, 8217, 8220, 8221, 8226, 8211, 8212, 1705, 8482, 1681, 8250, 339, 8204, 8205, 1722,
  160, 1548, 162, 163, 164, 165, 166, 167, 168, 169, 1726, 171, 172, 173, 174, 175,
  176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 1563, 187, 188, 189, 190, 1567,
  1729, 1569, 1570, 1571, 1572, 1573, 1574, 1575, 1576, 1577, 1578, 1579, 1580, 1581, 1582, 1583,
  1584, 1585, 1586, 1587, 1588, 1589, 1590, 215, 1591, 1592, 1593, 1594, 1600, 1601, 1602, 1603,
  224, 1604, 226, 1605, 1606, 1607, 1608, 231, 232, 233, 234, 235, 1609, 1610, 238, 239,
  1611, 1612, 1613, 1614, 244, 1615, 1616, 247, 1617, 249, 1618, 251, 252, 8206, 8207, 1746,
};
inline constexpr std::array<std::uint16_t, 128> kCP1251 = {
  1026, 1027, 8218, 1107, 8222, 8230, 8224, 8225, 8364, 8240, 1033, 8249, 1034, 1036, 1035, 1039,
  1106, 8216, 8217, 8220, 8221, 8226, 8211, 8212, 0, 8482, 1113, 8250, 1114, 1116, 1115, 1119,
  160, 1038, 1118, 1032, 164, 1168, 166, 167, 1025, 169, 1028, 171, 172, 173, 174, 1031,
  176, 177, 1030, 1110, 1169, 181, 182, 183, 1105, 8470, 1108, 187, 1112, 1029, 1109, 1111,
  1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055,
  1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063, 1064, 1065, 1066, 1067, 1068, 1069, 1070, 1071,
  1072, 1073, 1074, 1075, 1076, 1077, 1078, 1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087,
  1088, 1089, 1090, 1091, 1092, 1093, 1094, 1095, 1096, 1097, 1098, 1099, 1100, 1101, 1102, 1103,
};
inline constexpr std::array<std::uint16_t, 128> kCP1253 = {
  8364, 0, 8218, 402, 8222, 8230, 8224, 8225, 0, 8240, 0, 8249, 0, 0, 0, 0,
  0, 8216, 8217, 8220, 8221, 8226, 8211, 8212, 0, 8482, 0, 8250, 0, 0, 0, 0,
  160, 901, 902, 163, 164, 165, 166, 167, 168, 169, 0, 171, 172, 173, 174, 8213,
  176, 177, 178, 179, 900, 181, 182, 183, 904, 905, 906, 187, 908, 189, 910, 911,
  912, 913, 914, 915, 916, 917, 918, 919, 920, 921, 922, 923, 924, 925, 926, 927,
  928, 929, 0, 931, 932, 933, 934, 935, 936, 937, 938, 939, 940, 941, 942, 943,
  944, 945, 946, 947, 948, 949, 950, 951, 952, 953, 954, 955, 956, 957, 958, 959,
  960, 961, 962, 963, 964, 965, 966, 967, 968, 969, 970, 971, 972, 973, 974, 0,
};
inline constexpr std::array<std::uint16_t, 128> kCP874 = {
  8364, 0, 0, 0, 0, 8230, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 8216, 8217, 8220, 8221, 8226, 8211, 8212, 0, 0, 0, 0, 0, 0, 0, 0,
  160, 3585, 3586, 3587, 3588, 3589, 3590, 3591, 3592, 3593, 3594, 3595, 3596, 3597, 3598, 3599,
  3600, 3601, 3602, 3603, 3604, 3605, 3606, 3607, 3608, 3609, 3610, 3611, 3612, 3613, 3614, 3615,
  3616, 3617, 3618, 3619, 3620, 3621, 3622, 3623, 3624, 3625, 3626, 3627, 3628, 3629, 3630, 3631,
  3632, 3633, 3634, 3635, 3636, 3637, 3638, 3639, 3640, 3641, 3642, 0, 0, 0, 0, 3647,
  3648, 3649, 3650, 3651, 3652, 3653, 3654, 3655, 3656, 3657, 3658, 3659, 3660, 3661, 3662, 3663,
  3664, 3665, 3666, 3667, 3668, 3669, 3670, 3671, 3672, 3673, 3674, 3675, 0, 0, 0, 0,
};
// clang-format on

// decode_codepage maps `raw` (bytes) through a Windows codepage table to
// UTF-8. Undefined bytes become U+FFFD, matching Go's charmap decoders.
std::string decode_codepage(const std::string& raw, const std::array<std::uint16_t, 128>& table) {
  std::vector<char32_t> runes;
  runes.reserve(raw.size());
  for (const unsigned char b : raw) {
    if (b < 0x80) {
      runes.push_back(b);
    } else {
      const std::uint16_t cp = table[b - 0x80];
      runes.push_back(cp == 0 ? 0xFFFD : static_cast<char32_t>(cp));
    }
  }
  return utf8_encode(runes);
}

// is_letter matches Go's unicode.IsLetter for every codepoint the five
// legacy codepages (cp1255/cp1256/cp1251/cp1253/cp874) can produce above the
// caller's cp > 0x24F score filter: Greek, Cyrillic, Hebrew, Arabic, Thai
// letters plus U+02C6 (modifier circumflex, from cp1255/cp1256 byte 0x88).
// Exact ranges verified exhaustively against Python unicodedata (zero
// mismatches); combining marks (U+05C2, U+064B-U+064D, U+0E31) and symbols
// (U+0482, U+0483-U+0489) are deliberately excluded.
bool is_letter(char32_t cp) {
  if (cp <= 0x24F) return false;
  return cp == 0x2C6 ||                       // modifier circumflex (Lm)
         (cp >= 0x0370 && cp <= 0x0374) ||    // Greek
         (cp >= 0x0376 && cp <= 0x0377) || (cp >= 0x037A && cp <= 0x037D) ||
         cp == 0x037F || cp == 0x0386 || (cp >= 0x0388 && cp <= 0x038A) ||
         cp == 0x038C || (cp >= 0x038E && cp <= 0x03A1) ||
         (cp >= 0x03A3 && cp <= 0x03F5) || (cp >= 0x03F7 && cp <= 0x03FF) ||
         (cp >= 0x0400 && cp <= 0x0481) ||    // Cyrillic (not 0x482-0x489 marks)
         (cp >= 0x048A && cp <= 0x04FF) ||
         (cp >= 0x05D0 && cp <= 0x05EA) ||    // Hebrew (not 0x5EB-0x5EE)
         (cp >= 0x05EF && cp <= 0x05F2) ||
         (cp >= 0x0620 && cp <= 0x064A) ||    // Arabic (not 0x64B-0x64D marks)
         (cp >= 0x066E && cp <= 0x066F) || (cp >= 0x0671 && cp <= 0x06D3) ||
         cp == 0x06D5 || (cp >= 0x06E5 && cp <= 0x06E6) ||
         (cp >= 0x06EE && cp <= 0x06EF) || (cp >= 0x06FA && cp <= 0x06FC) ||
         cp == 0x06FF ||
         (cp >= 0x0E01 && cp <= 0x0E30) ||    // Thai (not 0xE31)
         (cp >= 0x0E32 && cp <= 0x0E33) || (cp >= 0x0E40 && cp <= 0x0E46);
}

// sanitize_tag is a 1:1 port of cliamp playlist/encoding.go sanitizeTag:
// detects mojibake from legacy codepages (high density of Latin-1 supplement
// runes and no runes beyond U+00FF), reverses the Latin-1 decode to recover
// the raw tag bytes, re-validates UTF-8, then tries the five Windows
// codepages and keeps the one that yields the most non-Latin letters.
std::string sanitize_tag(std::string_view s) {
  if (s.empty()) return std::string(s);

  const std::vector<char32_t> runes = utf8_decode(s);
  const std::size_t total = runes.size();
  std::size_t high = 0;
  for (const char32_t r : runes) {
    if (r >= 0x80 && r <= 0xFF) ++high;
  }
  if (total == 0 || high * 3 < total) return std::string(s);

  // Reverse the Latin-1 decode to recover the original tag bytes.
  std::string raw;
  raw.reserve(total);
  for (const char32_t r : runes) {
    if (r > 0xFF) return std::string(s);  // not simple mojibake
    raw.push_back(static_cast<char>(r));
  }

  // The original bytes might be valid UTF-8 that was double-decoded.
  if (is_valid_utf8(raw)) return raw;

  // Try legacy codepages; pick the one that produces the most non-Latin
  // letters (order matches Go's legacyEncodings).
  static const std::array<const std::array<std::uint16_t, 128>*, 5> kCodepages = {
      &kCP1255, &kCP1256, &kCP1251, &kCP1253, &kCP874};
  std::string best;
  int best_score = 0;
  for (const auto* table : kCodepages) {
    const std::string text = decode_codepage(raw, *table);
    int score = 0;
    for (const char32_t r : utf8_decode(text)) {
      if (is_letter(r) && r > 0x24F) ++score;
    }
    if (score > best_score) {
      best_score = score;
      best = text;
    }
  }
  if (best_score > 0) return best;
  return std::string(s);
}

// ---------------------------------------------------------------------------
// Album art cache (port of cliamp tags.go cacheAlbumArt / cleanupAlbumArtCache
// / fileURL / normalizedPictureExt). Files are named
// hex(sha256(picture data)).<ext> under <data_dir()>/album-art and surfaced
// as file:// URLs.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kAlbumArtCacheDir = "album-art";
inline constexpr std::int64_t kAlbumArtCacheMaxBytes = 100LL << 20;

// ext_for_mime is cliamp normalizedPictureExt reduced to the MIME branch
// (TagLib pictures carry a MIME type rather than an extension).
std::string ext_for_mime(std::string_view mime) {
  if (mime == "image/jpeg" || mime == "image/jpg") return "jpg";
  if (mime == "image/png") return "png";
  if (mime == "image/gif") return "gif";
  if (mime == "image/webp") return "webp";
  if (mime == "image/bmp") return "bmp";
  if (mime == "image/tiff") return "tiff";
  return "jpg";
}

// percent_encode_path mirrors Go's url.URL{Scheme:"file", Path:...}.String():
// the path is escaped per RFC 3986 pchar + '/' (Go escape(encodePath)), i.e.
// everything except unreserved, sub-delims, ':', '@' and '/' becomes %XX
// (uppercase hex, UTF-8 bytes).
std::string percent_encode_path(std::string_view p) {
  std::string out;
  out.reserve(p.size());
  for (const unsigned char c : p) {
    const bool alnum =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum || c == '-' || c == '_' || c == '.' || c == '~' || c == '!' ||
        c == '\'' || c == '(' || c == ')' || c == '*' || c == '$' || c == '&' ||
        c == '+' || c == ',' || c == '/' || c == ':' || c == ';' || c == '=' ||
        c == '?' || c == '@') {
      out.push_back(static_cast<char>(c));
    } else {
      static constexpr char kHex[] = "0123456789ABCDEF";
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

// file_url is cliamp tags.go fileURL: absolute path -> RFC 8089 file:// URL.
std::string file_url(const fs::path& path) {
  std::string slashed = fs::absolute(path).string();
  // Non-absolute paths get a leading slash (no-op for absolute inputs on
  // Linux; kept for Go fidelity).
  if (slashed.empty() || slashed.front() != '/') {
    slashed.insert(slashed.begin(), '/');
  }
  return "file://" + percent_encode_path(slashed);
}

struct cached_file {
  fs::path           path;
  std::int64_t       size;
  fs::file_time_type mtime;
};

// cleanup_album_art_cache removes the oldest (by mtime) cache files until the
// directory totals <= max_bytes; keep_path is never removed. Port of cliamp
// cleanupAlbumArtCache.
void cleanup_album_art_cache(const fs::path& dir, std::int64_t max_bytes,
                             const fs::path& keep_path) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return;
  const fs::directory_iterator end;
  std::vector<cached_file> files;
  std::int64_t total = 0;
  for (; it != end; it.increment(ec)) {
    if (ec) return;
    const fs::directory_entry& e = *it;
    if (!e.is_regular_file(ec)) continue;
    const std::int64_t size = static_cast<std::int64_t>(e.file_size(ec));
    if (ec) continue;
    total += size;
    files.push_back({e.path(), size, e.last_write_time(ec)});
  }
  if (total <= max_bytes) return;
  std::sort(files.begin(), files.end(),
            [](const cached_file& a, const cached_file& b) { return a.mtime < b.mtime; });
  for (const cached_file& f : files) {
    if (total <= max_bytes) return;
    if (f.path == keep_path) continue;
    if (fs::remove(f.path, ec)) total -= f.size;
  }
}

// cache_album_art writes picture bytes to the art cache and returns the
// file:// URL (cliamp cacheAlbumArt). Returns "" when nothing is cached.
std::string cache_album_art(const TagLib::ByteVector& data, std::string_view mime) {
  if (data.isEmpty()) return "";
  auto data_dir = foundation::data_dir();
  if (!data_dir) return "";
  const fs::path art_dir = *data_dir / kAlbumArtCacheDir;
  std::error_code ec;
  fs::create_directories(art_dir, ec);
  if (ec) return "";

  const std::string name = sha256_hex(data.data(), data.size()) + "." + ext_for_mime(mime);
  const fs::path path = art_dir / name;
  if (fs::exists(path)) {
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    cleanup_album_art_cache(art_dir, kAlbumArtCacheMaxBytes, path);
    return file_url(path);
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return "";
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) return "";
  // Match Go's 0644 create mode (std::ofstream would use 0666 & umask).
  fs::permissions(path, fs::perms(0644), ec);
  cleanup_album_art_cache(art_dir, kAlbumArtCacheMaxBytes, path);
  return file_url(path);
}

// ---------------------------------------------------------------------------
// TagLib extraction. Lyrics and pictures are format-specific:
//   ID3v2 (MP3, DSF):   USLT/ULT frame, APIC/PIC frame
//   MP4:                \xa9lyr atom, covr atom
//   Vorbis comments (FLAC, Ogg Vorbis/Opus/Speex): LYRICS field,
//                       METADATA_BLOCK_PICTURE / FLAC picture blocks
// ---------------------------------------------------------------------------

struct ArtData {
  TagLib::ByteVector data;
  std::string        mime;
};

// id3v2_tag_of returns the ID3v2 tag for files that carry one.
TagLib::ID3v2::Tag* id3v2_tag_of(TagLib::File* file) {
  if (auto* mp3 = dynamic_cast<TagLib::MPEG::File*>(file)) {
    return mp3->hasID3v2Tag() ? mp3->ID3v2Tag() : nullptr;
  }
  if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(file)) {
    return dsf->tag();
  }
  return nullptr;
}

// xiph_comment_of returns the Vorbis comment for FLAC / Ogg-family files.
TagLib::Ogg::XiphComment* xiph_comment_of(TagLib::File* file) {
  if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file)) {
    return flac->xiphComment();
  }
  if (auto* vorbis = dynamic_cast<TagLib::Ogg::Vorbis::File*>(file)) {
    return static_cast<TagLib::Ogg::XiphComment*>(vorbis->tag());
  }
  if (auto* opus = dynamic_cast<TagLib::Ogg::Opus::File*>(file)) {
    return opus->tag();
  }
  if (auto* speex = dynamic_cast<TagLib::Ogg::Speex::File*>(file)) {
    return speex->tag();
  }
  return nullptr;
}

std::string id3v2_lyrics(TagLib::ID3v2::Tag* tag) {
  if (tag == nullptr) return "";
  for (const char* id : {"USLT", "ULT"}) {  // ULT = ID3v2.2 spelling
    const TagLib::ID3v2::FrameList frames = tag->frameList(id);
    if (frames.isEmpty()) continue;
    const auto* frame = static_cast<const TagLib::ID3v2::UnsynchronizedLyricsFrame*>(frames.front());
    return frame->text().to8Bit(true);
  }
  return "";
}

std::string xiph_lyrics(TagLib::Ogg::XiphComment* comment) {
  if (comment == nullptr) return "";
  for (const auto& [key, values] : comment->fieldListMap()) {
    std::string lower;
    lower.reserve(key.size());
    for (const char c : key.to8Bit(true)) {
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "lyrics" && !values.isEmpty()) {
      return values.front().to8Bit(true);
    }
  }
  return "";
}

// extract_lyrics is the port of dhowden/tag Lyrics(): USLT for ID3v2,
// \xa9lyr for MP4, "lyrics" for Vorbis comments.
std::string extract_lyrics(TagLib::File* file) {
  if (TagLib::ID3v2::Tag* tag = id3v2_tag_of(file); tag != nullptr) {
    return id3v2_lyrics(tag);
  }
  if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
    if (mp4->tag() != nullptr && mp4->tag()->contains("\xa9lyr")) {
      const TagLib::StringList values = mp4->tag()->item("\xa9lyr").toStringList();
      if (!values.isEmpty()) return values.front().to8Bit(true);
    }
    return "";
  }
  return xiph_lyrics(xiph_comment_of(file));
}

std::optional<ArtData> id3v2_picture(TagLib::ID3v2::Tag* tag) {
  if (tag == nullptr) return std::nullopt;
  for (const char* id : {"APIC", "PIC"}) {  // PIC = ID3v2.2 spelling
    const TagLib::ID3v2::FrameList frames = tag->frameList(id);
    if (frames.isEmpty()) continue;
    const auto* frame = static_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
    return ArtData{frame->picture(), frame->mimeType().to8Bit(true)};
  }
  return std::nullopt;
}

// extract_picture returns the first embedded picture (dhowden/tag Picture()
// semantics: one picture, first one wins).
std::optional<ArtData> extract_picture(TagLib::File* file) {
  if (std::optional<ArtData> art = id3v2_picture(id3v2_tag_of(file)); art) {
    return art;
  }
  if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file)) {
    const TagLib::List<TagLib::FLAC::Picture*> pictures = flac->pictureList();
    if (!pictures.isEmpty()) {
      const TagLib::FLAC::Picture* pic = pictures.front();
      return ArtData{pic->data(), pic->mimeType().to8Bit(true)};
    }
  } else if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
    if (mp4->tag() != nullptr && mp4->tag()->contains("covr")) {
      const TagLib::MP4::CoverArtList arts = mp4->tag()->item("covr").toCoverArtList();
      if (!arts.isEmpty()) {
        std::string mime;
        switch (arts.front().format()) {
          case TagLib::MP4::CoverArt::JPEG: mime = "image/jpeg"; break;
          case TagLib::MP4::CoverArt::PNG:  mime = "image/png"; break;
          case TagLib::MP4::CoverArt::BMP:  mime = "image/bmp"; break;
          case TagLib::MP4::CoverArt::GIF:  mime = "image/gif"; break;
          default:                          mime = ""; break;
        }
        return ArtData{arts.front().data(), mime};
      }
    }
  } else if (TagLib::Ogg::XiphComment* xc = xiph_comment_of(file); xc != nullptr) {
    const TagLib::List<TagLib::FLAC::Picture*> pictures = xc->pictureList();
    if (!pictures.isEmpty()) {
      const TagLib::FLAC::Picture* pic = pictures.front();
      return ArtData{pic->data(), pic->mimeType().to8Bit(true)};
    }
  }
  return std::nullopt;
}

}  // namespace

// read_tags is the port of cliamp readTagsWithOptions(path, true): tag
// fields are trimmed and mojibake-sanitized exactly like Go, embedded lyrics
// are returned, and embedded album art is cached to disk and surfaced as a
// file:// URL. Opening or parsing failures yield an error (Go's fallback to
// filename parsing is the caller's job, cf. playlist.hpp track_from_path
// "caller-side").
std::expected<TagInfo, std::string> read_tags(std::string_view path) {
  const std::string p(path);

  std::error_code ec;
  if (!fs::exists(p, ec) || ec) {
    return std::unexpected{"open " + p + ": no such file or directory"};
  }

  TagLib::FileRef ref(p.c_str());
  if (ref.isNull() || ref.file() == nullptr || ref.tag() == nullptr) {
    return std::unexpected{"unable to read tags from " + p};
  }

  TagInfo info;
  const TagLib::Tag* tag = ref.tag();

  // Field order follows Go readTagsWithOptions: lyrics + art first, then the
  // text fields, so a caller-side title fallback can reuse lyrics/art.
  info.lyrics = sanitize_tag(trim_ascii(extract_lyrics(ref.file())));
  if (std::optional<ArtData> art = extract_picture(ref.file()); art) {
    info.art_cache_path = cache_album_art(art->data, art->mime);
  }

  info.title = sanitize_tag(tag->title().stripWhiteSpace().to8Bit(true));
  info.artist = sanitize_tag(tag->artist().stripWhiteSpace().to8Bit(true));
  info.album = sanitize_tag(tag->album().stripWhiteSpace().to8Bit(true));
  info.genre = sanitize_tag(tag->genre().stripWhiteSpace().to8Bit(true));
  info.year = static_cast<int>(tag->year());
  info.track_number = static_cast<int>(tag->track());

  if (const TagLib::AudioProperties* props = ref.audioProperties(); props != nullptr) {
    info.duration_secs = props->lengthInSeconds();
  }
  return info;
}

}  // namespace bootamp::playlist
