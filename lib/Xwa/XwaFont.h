#pragma once

// The fallback font (spec 10 and 10.1): outline glyphs for the characters
// the firmware's own fonts lack, in FONT and its styled sections FONT_BOLD,
// FONT_ITALIC and FONT_BOLD_ITALIC. Glyphs use the MATH_GLYPHS outline
// encoding, so the formula rasteriser draws them.
//
// Memory: a section keeps the first code point of every 64th range and a
// small cache of recent lookups; everything else is read through the
// archive's block cache when needed. Swedish Wikipedia's FONT (10,157
// glyphs) costs about 1 KB of RAM this way.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "XwaArchive.h"

namespace xwa {

class FontSection {
 public:
  // False when the section is absent or its tables are damaged.
  bool open(Archive& archive, uint32_t type);
  bool isOpen() const { return ar_ != nullptr; }
  uint16_t upem() const { return upem_; }
  int16_t ascender() const { return ascender_; }
  int16_t descender() const { return descender_; }
  uint32_t glyphCount() const { return glyphCount_; }
  uint32_t rangeCount() const { return rangeCount_; }
  // Range i: code points first..last are glyphs glyph, glyph + 1, ...
  bool range(uint32_t i, uint32_t& first, uint32_t& last, uint32_t& glyph);

  // The glyph for `cp`, or -1.
  int32_t find(uint32_t cp);
  // Advance in font units (0 on a read error).
  uint16_t advance(uint32_t glyph);
  // The glyph's bytes: i16 bbox[4], then commands up to END.
  bool glyph(uint32_t glyph, std::vector<uint8_t>& out);

 private:
  static constexpr uint32_t GROUP = 64;  // ranges per sparse index entry
  static constexpr uint32_t CACHE = 64;  // recent lookups kept
  struct Hit {
    uint32_t cp = UINT32_MAX;
    int32_t glyph = -1;
  };

  Archive* ar_ = nullptr;
  uint64_t base_ = 0;
  uint64_t length_ = 0;
  uint16_t upem_ = 1000;
  int16_t ascender_ = 0;
  int16_t descender_ = 0;
  uint32_t rangeCount_ = 0;
  uint32_t glyphCount_ = 0;
  uint64_t advancesAt_ = 0;  // within the section
  uint64_t offsetsAt_ = 0;
  uint64_t dataAt_ = 0;
  std::vector<uint32_t> sparse_;  // first code point of ranges 0, 64, 128, ...
  Hit cache_[CACHE];
};

// How one character is drawn in a style (10.1).
struct StyledGlyph {
  FontSection* section = nullptr;
  uint32_t glyph = 0;
  bool embolden = false;  // synthesise bold: offset the outline
  bool shear = false;     // synthesise italic: slant 12°
  uint16_t advance = 0;   // font units, widened when emboldened
};

// FONT and its styled sections, and the rules that pick a glyph for a style.
class FallbackFont {
 public:
  // Synthesised bold moves the outline out by this share of an em on every
  // side; synthesised italic slants by tan 12°.
  static constexpr float BOLD_OFFSET_EM = 0.025f;
  static constexpr float SHEAR = 0.21255656f;

  // False when the archive has no FONT (or it is damaged).
  bool open(Archive& archive);
  bool isOpen() const { return regular_.isOpen(); }
  uint16_t upem() const { return regular_.upem(); }
  int16_t ascender() const { return regular_.ascender(); }
  int16_t descender() const { return regular_.descender(); }
  // Whether FONT draws `cp` at all.
  bool has(uint32_t cp) { return regular_.find(cp) >= 0; }
  // The glyph for `cp` in a style. `prevUpright` is the slant decision of
  // the character before, which combining marks follow; it is updated.
  bool resolve(uint32_t cp, bool bold, bool italic, bool& prevUpright, StyledGlyph& out);
  // Which styled sections the archive has (for tests and the info screen).
  bool hasBold() const { return bold_.isOpen(); }
  bool hasItalic() const { return italic_.isOpen(); }
  bool hasBoldItalic() const { return boldItalic_.isOpen(); }

 private:
  FontSection regular_;
  FontSection bold_;
  FontSection italic_;
  FontSection boldItalic_;
};

// Characters synthesised italic leaves upright: scripts with no italic
// convention, symbols, and what Noto Sans Italic itself draws upright.
bool staysUpright(uint32_t cp);
// Combining marks (they follow their base's slant).
bool isCombining(uint32_t cp);

// Rewrites a glyph (the bytes glyph() returns) with its outline moved out
// by `offset` font units on every side, as synthesised bold. False on
// malformed data.
bool emboldenGlyph(const uint8_t* data, size_t len, float offset, std::vector<uint8_t>& out);

}  // namespace xwa
