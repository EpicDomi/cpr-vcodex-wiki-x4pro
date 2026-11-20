#pragma once

// XWA offline wiki archives, format version 0.4: constants and the small
// byte-level helpers every part of the reader shares. The format is defined
// by xwa-tool's docs/format-spec-v0.md; section numbers below refer to it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace xwa {

constexpr uint8_t MAGIC[8] = {'X', 'W', 'A', 0, '\r', '\n', 0x1A, '\n'};
constexpr uint16_t VERSION_MAJOR = 0;
constexpr uint16_t VERSION_MINOR = 4;
constexpr uint32_t BLOCK_SIZE = 4096;
constexpr uint32_t SUPERBLOCK_SIZE = 4096;
// Largest chunk a reader accepts (chunk_bits 15).
constexpr uint32_t MAX_CHUNK_BITS = 15;
constexpr uint32_t MIN_CHUNK_BITS = 9;
constexpr uint32_t MAX_CHUNK = 1u << MAX_CHUNK_BITS;
constexpr uint32_t NO_ARTICLE = 0xFFFFFFFFu;
constexpr int DAYS_IN_YEAR = 366;

// Section types (3.2).
namespace section {
constexpr uint32_t META = 1;
constexpr uint32_t ARTICLES = 2;
constexpr uint32_t TITLES = 3;
constexpr uint32_t TINDEX_LEAF = 4;
constexpr uint32_t TINDEX_INNER = 5;
constexpr uint32_t CHUNK_TABLE = 6;
constexpr uint32_t BLOBS = 7;
constexpr uint32_t IMAGE_TABLE = 8;
constexpr uint32_t MATH_GLYPHS = 9;
constexpr uint32_t ZSTD_DICT = 10;
constexpr uint32_t FONT = 11;
constexpr uint32_t WORD_INDEX = 12;
constexpr uint32_t FOLD = 13;
constexpr uint32_t DAYS = 14;
constexpr uint32_t FONT_BOLD = 15;
constexpr uint32_t FONT_ITALIC = 16;
constexpr uint32_t FONT_BOLD_ITALIC = 17;
}  // namespace section

// Required (3.3): a reader lacking one refuses the archive.
namespace required {
constexpr uint32_t ZSTD_TEXT = 1u << 0;
constexpr uint32_t MATH = 1u << 1;
constexpr uint32_t LINE_ART = 1u << 2;
constexpr uint32_t JPEG = 1u << 3;
}  // namespace required

namespace optional {
constexpr uint32_t FONT = 1u << 0;
constexpr uint32_t WORD_INDEX = 1u << 1;
constexpr uint32_t POPULARITY = 1u << 2;
constexpr uint32_t HD_IMAGES = 1u << 3;
}  // namespace optional

// ARTICLES record flags (5.1).
namespace article_flag {
constexpr uint16_t INFOBOX = 1u << 0;
constexpr uint16_t TABLES = 1u << 1;
constexpr uint16_t MATH = 1u << 2;
constexpr uint16_t IMAGES = 1u << 3;
constexpr uint16_t DISAMBIGUATION = 1u << 4;
constexpr uint16_t LIST = 1u << 5;
}  // namespace article_flag

// Title index entry flags (6.2).
namespace entry_flag {
constexpr uint8_t REDIRECT = 1u << 0;
constexpr uint8_t ANCHOR = 1u << 1;
}  // namespace entry_flag

// Image records (9.1, 9.2).
namespace image_encoding {
constexpr uint8_t JPEG = 1;
constexpr uint8_t LINE_ART = 2;
}  // namespace image_encoding
namespace image_kind {
constexpr uint8_t PHOTO = 0;
constexpr uint8_t LINE_ART = 1;
constexpr uint8_t ICON = 2;
}  // namespace image_kind

// Block opcodes (7.3).
namespace block_op {
constexpr uint8_t PARA = 0x01;
constexpr uint8_t HEADING = 0x02;
constexpr uint8_t LIST_START = 0x03;
constexpr uint8_t LIST_ITEM = 0x04;
constexpr uint8_t LIST_END = 0x05;
constexpr uint8_t QUOTE_START = 0x06;
constexpr uint8_t QUOTE_END = 0x07;
constexpr uint8_t PRE = 0x08;
constexpr uint8_t RULE = 0x09;
constexpr uint8_t FIGURE = 0x0A;
constexpr uint8_t MATH_BLOCK = 0x0B;
constexpr uint8_t TABLE_REF = 0x0C;
constexpr uint8_t INFOBOX_REF = 0x0D;
constexpr uint8_t GALLERY = 0x0E;
constexpr uint8_t EXT = 0x7F;
}  // namespace block_op

// Inline opcodes (7.3). 0x01-0x7F are short TEXT ops (op = byte length).
namespace inline_op {
constexpr uint8_t TEXT_LONG = 0x80;
constexpr uint8_t STYLE_PUSH = 0x81;
constexpr uint8_t STYLE_POP = 0x82;
constexpr uint8_t LINK_START = 0x83;
constexpr uint8_t LINK_END = 0x84;
constexpr uint8_t MATH = 0x85;
constexpr uint8_t ICON = 0x86;
constexpr uint8_t BREAK = 0x87;
constexpr uint8_t NOTE_REF = 0x88;
constexpr uint8_t LINK_ANCHOR = 0x89;
constexpr uint8_t EXT = 0xFF;
}  // namespace inline_op

enum class Style : uint8_t { Bold = 0, Italic = 1, Subscript = 2, Superscript = 3, Monospace = 4, SmallCaps = 5 };
enum class ListKind : uint8_t { Bullet = 0, Number = 1, Definition = 2 };
enum class InfoboxRowKind : uint8_t { Header = 0, Image = 1, Field = 2, Full = 3 };

// Lists and quotes nest at most this deep, and at most this many styles and
// links are open at once in a run (7.3).
constexpr int MAX_DEPTH = 64;

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le24(const uint8_t* p) { return p[0] | (p[1] << 8) | (static_cast<uint32_t>(p[2]) << 16); }
inline uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le48(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le16(p + 4)) << 32); }
inline uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

// Unsigned LEB128 from [p, end). Advances p; false on truncation or overflow.
inline bool getVarint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (p >= end) return false;
    const uint8_t b = *p++;
    v |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      out = v;
      return true;
    }
  }
  return false;
}

inline bool getVarint32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
  uint64_t v = 0;
  if (!getVarint(p, end, v) || v > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

inline int64_t unzigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }

// Large buffers (chunks, decoded images): PSRAM where the board has it.
inline void* bigAlloc(size_t bytes) {
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  if (void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
#endif
  return std::malloc(bytes);
}
inline void bigFree(void* p) { std::free(p); }

// Appends a code point as UTF-8.
template <typename String>
inline void appendUtf8(String& out, uint32_t cp) {
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

// Decodes one code point from [p, end); returns 0xFFFD and advances one byte
// on malformed input. Advances p past the sequence.
inline uint32_t nextUtf8(const uint8_t*& p, const uint8_t* end) {
  const uint8_t b = *p++;
  if (b < 0x80) return b;
  int extra = 0;
  uint32_t cp = 0;
  if ((b & 0xE0) == 0xC0) {
    extra = 1;
    cp = b & 0x1F;
  } else if ((b & 0xF0) == 0xE0) {
    extra = 2;
    cp = b & 0x0F;
  } else if ((b & 0xF8) == 0xF0) {
    extra = 3;
    cp = b & 0x07;
  } else {
    return 0xFFFD;
  }
  if (end - p < extra) return 0xFFFD;
  for (int i = 0; i < extra; i++) {
    if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  p += extra;
  return cp;
}

}  // namespace xwa
