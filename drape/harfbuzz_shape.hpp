#pragma once

//#include "base/buffer_vector.hpp"

#include <vector>

#include <hb.h>

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
  int32_t m_length;    // Offset to the segment end in the string.
  hb_script_t m_script;
  hb_direction_t m_direction;
  TextRun(int32_t start, int32_t length, hb_script_t script, hb_direction_t direction)
  : m_start(start), m_length(length), m_script(script), m_direction((direction)) {}
};

struct TextRuns
{
  std::u16string text;
  std::vector<TextRun> substrings;
  std::vector<size_t> runOrder;
};

struct GlyphMetrics
{
  uint16_t glyphId;
  float m_xAdvance;
  float m_yAdvance;
  float m_xOffset;
  float m_yOffset;
};

struct TextMetrics
{
  float m_width;
  float m_height;
  std::vector<GlyphMetrics> m_glyphs;
};

// Shapes a single line of text without newline \r or \n characters.
// Any line breaking/trimming should be done by the caller.
TextRuns ItemizeText(std::string_view utf8);
void ReorderRTL(TextRuns & runs);
TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, int8_t lang);
TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, char const * lang);
}  // namespace text_shape