#pragma once

//#include "base/buffer_vector.hpp"

#include <vector>

#include <harfbuzz/hb.h>

namespace dp { class GlyphManager; }

// Now the font is autodetected from the codepoint.
// TODO:(AB): Pass custom fonts to render with a fallback.
struct FontParams {
  int pixelSize;
  int8_t lang;
};

// Input: "full string", fontSize
// Internal cache two layers:
// Lower layer stores individual runs:
// ["full", fontSize] => totalw, totalh, [[glyphid, x, y, offx, offy], [glyphid, x, y, offx, offy], ...]
// [" ", fontSize] => ...
// ["string", fontSize] => ...
// Upper one matches original string to individual runs:
// ["full string", fontSize] => [["full", fontsize], [" ", fontsize], ["string", fontsize]]
// Rendered glyph cache:
// map<glyphid, coords_on_texture> is_already_on_texture
// Layouter: total string width and height: sum(totalw), max(totalh), iterate glyphs with their offsets.

// Get string and font size
// Get text runs by dir, script, font?
// Shape each run with font size and font, store glyphs and offsets
//   Shaping:
//
// Render glyphs to atlas

namespace text_shape
{
struct TextRun
{
  // TODO(AB): Use 1 byte or 2 bytes.
  int32_t m_start;  // Offset to the segment start in the string.
  int32_t m_length;
  hb_script_t m_script;
  hb_direction_t m_direction;
  TextRun(int32_t start, int32_t length, hb_script_t script, hb_direction_t direction)
  : m_start(start), m_length(length), m_script(script), m_direction(direction) {}
};

struct TextRuns
{
  std::u16string text;
  std::vector<TextRun> substrings;
  // TODO(AB): Use indexes to order runs.
  //std::vector<size_t> runOrder;
};

struct GlyphMetrics
{
  int16_t m_font;
  uint16_t m_glyphId;
  // TODO(AB): Store original font units or floats?
  int32_t m_xOffset;
  int32_t m_yOffset;
  int32_t m_xAdvance;
  // yAdvance is used only in vertical text layouts.
};

struct TextMetrics
{
  int32_t m_width {0};
  std::vector<GlyphMetrics> m_glyphs;

  void AddGlyphMetrics(int16_t font, uint16_t glyphId, int32_t xOffset, int32_t yOffset, int32_t xAdvance)
  {
    m_glyphs.push_back({font, glyphId, xOffset, yOffset, xAdvance});
    m_width += xAdvance;
  }
};

using ShapeHarfbuzzBufferFn = std::function<void (strings::UniChar c, hb_buffer_t * hbBuffer, int pixelHeight, TextMetrics & out)>;

// Shapes a single line of text without newline \r or \n characters.
// Any line breaking/trimming should be done by the caller.
TextRuns ItemizeText(std::string_view utf8);
void ReorderRTL(TextRuns & runs);
TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, int8_t lang, ShapeHarfbuzzBufferFn && shapeFn);
TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, char const * lang, ShapeHarfbuzzBufferFn && shapeFn);
}  // namespace text_shape
