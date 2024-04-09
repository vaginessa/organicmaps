#include "drape/harfbuzz_shape.hpp"
#include "drape/glyph_manager.hpp"

#include "base/assert.hpp"
#include "base/math.hpp"
#include "base/string_utils.hpp"

#include <string>

#include <hb.h>
#include <hb-icu.h>
#include <unicode/ubidi.h>  // ubidi_open, ubidi_setPara
#include <unicode/uscript.h>  // UScriptCode
#include <unicode/utf16.h>  // U16_NEXT

namespace text_shape
{
namespace
{
// The maximum number of scripts a Unicode character can belong to. This value
// is arbitrarily chosen to be a good limit because it is unlikely for a single
// character to belong to more scripts.
constexpr size_t kMaxScripts = 32;

//UBiDiLevel GetParagraphLevelForGivenText(const std::u16string& text) {
//  const char16_t* string = text.c_str();
//  size_t const length = text.length();
//  size_t position = 0;
//  while (position < length) {
//    UChar32 character;
//    size_t next_position = position;
//    U16_NEXT(string, next_position, length, character);
//
//    int32_t const property = u_getIntPropertyValue(character, UCHAR_BIDI_CLASS);
//    switch (property) {
//    case U_RIGHT_TO_LEFT:
//    case U_RIGHT_TO_LEFT_ARABIC:
//    case U_RIGHT_TO_LEFT_EMBEDDING:
//    case U_RIGHT_TO_LEFT_OVERRIDE:
//      return 1;  // Highest RTL level.
//
//    case U_LEFT_TO_RIGHT:
//    case U_LEFT_TO_RIGHT_EMBEDDING:
//    case U_LEFT_TO_RIGHT_OVERRIDE:
//      return 0;  // Highest LTR level.
//
//    default: position = next_position;
//    }
//  }
//  return 0;  // Highest LTR level.
//}

// Writes the script and the script extensions of the Unicode |codepoint|.
// Returns the number of written scripts.
size_t GetScriptExtensions(UChar32 codepoint, UScriptCode* scripts) {
  // Fill |scripts| with the script extensions.
  UErrorCode icu_error = U_ZERO_ERROR;
  size_t const count = uscript_getScriptExtensions(codepoint, scripts, kMaxScripts, &icu_error);
  if (U_FAILURE(icu_error))
    return 0;

  return count;
}

// Intersects the script extensions set of |codepoint| with |result| and writes
// to |result|, reading and updating |resultSize|. The output |result| will be
// a subset of the input |result| (thus |resultSize| can only be smaller).
void ScriptSetIntersect(UChar32 codepoint, UScriptCode* result, size_t* resultSize) {
  // Each codepoint has a Script property and a Script Extensions (Scx)
  // property.
  //
  // The implicit Script property values 'Common' and 'Inherited' indicate that
  // a codepoint is widely used in many scripts, rather than being associated
  // to a specific script.
  //
  // However, some codepoints that are assigned a value of 'Common' or
  // 'Inherited' are not commonly used with all scripts, but rather only with a
  // limited set of scripts. The Script Extension property is used to specify
  // the set of script which borrow the codepoint.
  //
  // Calls to GetScriptExtensions(...) return the set of scripts where the
  // codepoints can be used.
  // (see table 7 from http://www.unicode.org/reports/tr24/tr24-29.html)
  //
  //     Script       Script Extensions ->  Results
  //  1) Common       {Common}          ->  {Common}
  //     Inherited    {Inherited}       ->  {Inherited}
  //  2) Latin        {Latn}            ->  {Latn}
  //     Inherited    {Latn}            ->  {Latn}
  //  3) Common       {Hira Kana}       ->  {Hira Kana}
  //     Inherited    {Hira Kana}       ->  {Hira Kana}
  //  4) Devanagari   {Deva Dogr Kthi Mahj}  ->  {Deva Dogr Kthi Mahj}
  //     Myanmar      {Cakm Mymr Tale}  ->  {Cakm Mymr Tale}
  //
  // For most of the codepoints, the script extensions set contains only one
  // element. For CJK codepoints, it's common to see 3-4 scripts. For really
  // rare cases, the set can go above 20 scripts.
  UScriptCode scripts[kMaxScripts] = { USCRIPT_INVALID_CODE };
  size_t const count = GetScriptExtensions(codepoint, scripts);

  // Implicit script 'inherited' is inheriting scripts from preceding codepoint.
  if (count == 1 && scripts[0] == USCRIPT_INHERITED)
    return;

  auto const contains = [&scripts, count](UScriptCode code)
  {
    for (size_t i = 0; i < count; ++i)
      if (scripts[i] == code)
        return true;

    return false;
  };

  // Perform the intersection of both script set.
  ASSERT(!contains(USCRIPT_INHERITED), ());
  size_t outSize = 0;
  for (size_t i = 0; i < *resultSize; ++i)
  {
    auto const current = result[i];
    if (contains(current))
      result[outSize++] = current;
  }

  *resultSize = outSize;
}

// The CharIterator classes iterate through the characters in UTF8 and
// UTF16 strings.  Example usage:
//
//   for (UTF8CharIterator iter(str); !iter.end(); iter.Advance()) {
//     VLOG(1) << iter.get();
//   }
class UTF16CharIterator {
public:
  // Requires |str| to live as long as the UTF16CharIterator does.
  explicit UTF16CharIterator(std::u16string_view str) : str_(str),
        array_pos_{0},
        next_pos_{0},
        char_{0} {
    // This has the side-effect of advancing |next_pos_|.
    if (array_pos_ < str_.length())
      ReadChar();
  }

  UTF16CharIterator(UTF16CharIterator&& to_move) = default;
  UTF16CharIterator& operator=(UTF16CharIterator&& to_move) = default;

  UTF16CharIterator(const UTF16CharIterator&) = delete;
  UTF16CharIterator operator=(const UTF16CharIterator&) = delete;

  // Return the starting array index of the current character within the
  // string.
  size_t array_pos() const { return array_pos_; }

  // Returns the code point at the current position.
  int32_t get() const { return char_; }

  // Returns true if we're at the end of the string.
  bool end() const { return array_pos_ == str_.length(); }

  // Advances to the next actual character.  Returns false if we're at the
  // end of the string.
  bool Advance() {
    if (array_pos_ >= str_.length())
      return false;

    array_pos_ = next_pos_;
    if (next_pos_ < str_.length())
      ReadChar();

    return true;
  }

private:

  // Fills in the current character we found and advances to the next
  // character, updating all flags as necessary.
  void ReadChar()
  {
    // This is actually a huge macro, so is worth having in a separate function.
    // TODO(AB): Replace by utf8::next16
    U16_NEXT(str_.data(), next_pos_, str_.length(), char_);
  }
  // The string we're iterating over.
  std::u16string_view str_;
  // Array index.
  size_t array_pos_{0};
  // The next array index.
  size_t next_pos_;
  // The current character.
  int32_t char_;
};
}  // namespace

// Find the longest sequence of characters from 0 and up to |length| that have
// at least one common UScriptCode value. Writes the common script value to
// |script| and returns the length of the sequence. Takes the characters' script
// extensions into account. http://www.unicode.org/reports/tr24/#ScriptX
//
// Consider 3 characters with the script values {Kana}, {Hira, Kana}, {Kana}.
// Without script extensions only the first script in each set would be taken
// into account, resulting in 3 runs where 1 would be enough.
size_t ScriptInterval(std::u16string const & text, int32_t start, size_t length, UScriptCode* script)
{
  ASSERT_GREATER(length, 0U, ());
  UScriptCode scripts[kMaxScripts] = { USCRIPT_INVALID_CODE };

  UTF16CharIterator iterator{std::u16string_view{text.data() + start, length}};
  size_t scriptsSize = GetScriptExtensions(iterator.get(), scripts);

  while (iterator.Advance())
  {
    ScriptSetIntersect(iterator.get(), scripts, &scriptsSize);
    if (scriptsSize == 0U)
    {
      length = iterator.array_pos();
      break;
    }
  }

  *script = scripts[0];
  return length;
}

// A copy of hb_icu_script_to_script to avoid direct ICU dependency.
hb_script_t ICUScriptToHarfbuzzScript(UScriptCode script)
{
  if (script == USCRIPT_INVALID_CODE)
    return HB_SCRIPT_INVALID;
  return hb_script_from_string(uscript_getShortName (script), -1);
}

void GetSingleTextLineRuns(TextRuns & runs)
{
  auto const & text = runs.text;
  auto const textLength = static_cast<int32_t>(text.length());

  // Deliberately not checking for nullptr.
  thread_local UBiDi * const bidi = ubidi_open();
  UErrorCode error = U_ZERO_ERROR;
  ::ubidi_setPara(bidi, text.data(), textLength, UBIDI_DEFAULT_LTR, nullptr, &error);
  if (U_FAILURE(error))
  {
    LOG(LERROR, ("ubidi_setPara failed with code", error));
    auto constexpr kDefaultFont = 0;
    runs.substrings.emplace_back(0, 0, HB_SCRIPT_UNKNOWN, HB_DIRECTION_INVALID);
    return;
  }

  // Split the original text by logical runs, then each logical run by common
  // script and each sequence at special characters and style boundaries. This
  // invariant holds: bidi_run_start <= script_run_start <= breakingRunStart
  // <= breakingRunEnd <= script_run_end <= bidi_run_end
  for (int32_t bidiRunStart = 0; bidiRunStart < textLength;)
  {
    // Determine the longest logical run (e.g. same bidi direction) from this point.
    int32_t bidiRunBreak = 0;
    UBiDiLevel bidiLevel = 0;
    ::ubidi_getLogicalRun(bidi, bidiRunStart, &bidiRunBreak, &bidiLevel);
    int32_t const bidiRunEnd = bidiRunBreak;
    ASSERT_LESS(bidiRunStart, bidiRunEnd, ());

    for (int32_t scriptRunStart = bidiRunStart; scriptRunStart < bidiRunEnd;)
    {
      // Find the longest sequence of characters that have at least one common UScriptCode value.
      UScriptCode script = USCRIPT_INVALID_CODE;
      size_t const scriptRunEnd = ScriptInterval(runs.text, scriptRunStart, bidiRunEnd - scriptRunStart, &script) + scriptRunStart;
      ASSERT_LESS(scriptRunStart, base::asserted_cast<int32_t>(scriptRunEnd), ());

      // TODO(AB): May need to break on different unicode blocks, parentheses, and control chars (spaces).

//      for (size_t breakingRunStart = scriptRunStart; breakingRunStart < scriptRunEnd;) {
//        // Find the break boundary for style. The style won't break a grapheme
//        // since the style of the first character is applied to the whole
//        // grapheme.
//        style.IncrementToPosition(breakingRunStart);
//        size_t text_style_end = style.GetTextBreakingRange().end();

        // Break runs at certain characters that need to be rendered separately
        // to prevent an unusual character from forcing a fallback font on the
        // entire run. After script intersection, many codepoints end up in the
        // script COMMON but can't be rendered together.
//        size_t breakingRunEnd = FindRunBreakingCharacter(
//            text, script, breakingRunStart, text_style_end, scriptRunEnd);
//
//        DCHECK_LT(breakingRunStart, breakingRunEnd);
//        DCHECK(IsValidCodePointIndex(text, breakingRunEnd));

        // Set the font params for the current run for the current run break.
//        internal::TextRunHarfBuzz::FontParams font_params =
//            CreateFontParams(primary_font, bidiLevel, script);

        // Create the current run from [breakingRunStart, breakingRunEnd[.
        //auto run = std::make_unique<internal::TextRunHarfBuzz>(primary_font);
        //run->range = Range(breakingRunStart, breakingRunEnd);
        //run->range = Range(scriptRunStart, scriptRunEnd);
//        TextRun run;
//        run.start = scriptRunStart;
//        run.end = scriptRunEnd;
//        run.script = ICUScriptToHarfbuzzScript(script);
//        runs.push_back(run);
      // TODO(AB): Support vertical layouts.
      runs.substrings.emplace_back(scriptRunStart, scriptRunEnd - scriptRunStart, ICUScriptToHarfbuzzScript(script), bidiLevel & 0x01 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
      //runs.emplace_back(scriptRunStart, base::asserted_cast<int32_t>(scriptRunEnd), ICUScriptToHarfbuzzScript(script), 0);

        // Add the created run to the set of runs.
        //(*out_commonized_run_map)[font_params].push_back(run.get());
        //out_run_list->Add(std::move(run));

//        // Move to the next run.
//        breakingRunStart = breakingRunEnd;
//      }

      // Move to the next script sequence.
        scriptRunStart = static_cast<int32_t>(scriptRunEnd);
    }

    // Move to the next direction sequence.
    bidiRunStart = bidiRunEnd;
  }
}

hb_language_t OrganicMapsLanguageToHarfbuzzLanguage(int8_t lang)
{
    // TODO(AB): can langs be converted faster?
    auto const svLang = StringUtf8Multilang::GetLangByCode(lang);
    auto const hbLanguage = hb_language_from_string(svLang.data(), static_cast<int>(svLang.size()));
    if (hbLanguage == HB_LANGUAGE_INVALID)
      return hb_language_get_default();
    return hbLanguage;
}

// We treat HarfBuzz ints as 16.16 fixed-point.
static constexpr int kHbUnit1 = 1 << 16;

int FloatToHarfBuzzUnits(float value)
{
  auto floatRes = value * kHbUnit1;
  auto const intRes = static_cast<int>(floatRes);
  //ASSERT(math::AlmostEqualAbs(floatRes, static_cast<float>(intRes), 1.0-e20), ());
  return intRes;
}

float HarfBuzzUnitsToFloat(int value) {
    static constexpr float kFloatToHbRatio = 1.0f / kHbUnit1;
    return kFloatToHbRatio * static_cast<float>(value);
}

typedef int Font;
/*
hb_font_t* CreateHarfbuzzFont(Font const & font, int textSize, const FontRenderParams& params,
                               bool subpixel_rendering_suppressed) {
    // A cache from Skia font to harfbuzz typeface information.
    using TypefaceCache = base::LRUCache<SkFontID, TypefaceData>;

    constexpr int kTypefaceCacheSize = 64;
    static base::NoDestructor<TypefaceCache> face_caches(kTypefaceCacheSize);

    TypefaceCache* typeface_cache = face_caches.get();
    TypefaceCache::iterator typeface_data =
        typeface_cache->Get(skia_face->uniqueID());
    if (typeface_data == typeface_cache->end()) {
      TypefaceData new_typeface_data(skia_face);
      typeface_data = typeface_cache->Put(skia_face->uniqueID(),
                                          std::move(new_typeface_data));
    }

    DCHECK(typeface_data->second.face());
    hb_font_t* harfbuzz_font = hb_font_create(typeface_data->second.face());

    const int scale = FloatToHarfBuzzUnits(text_size);
    hb_font_set_scale(harfbuzz_font, scale, scale);
    FontData* hb_font_data = new FontData(typeface_data->second.glyphs());
    hb_font_data->font_.setTypeface(std::move(skia_face));
    hb_font_data->font_.setSize(text_size);
    // TODO(ckocagil): Do we need to update these params later?
    internal::ApplyRenderParams(params, subpixel_rendering_suppressed,
                                &hb_font_data->font_);
    hb_font_set_funcs(harfbuzz_font, g_font_funcs.Get().get(), hb_font_data,
                      DeleteByType<FontData>);
    hb_font_make_immutable(harfbuzz_font);
    return harfbuzz_font;
}
*/

void ShapeRunWithFont(FontParams const & fontParams, TextRun & run)
{
  // TODO(AB): set HB_BUFFER_FLAG_BOT for the beginning of rendered text.

  // In the current Drape implementation, each Unicode character is mapped to one font.
  // hb_font_t * hbFont = dp::GlyphManager::GetFontIndex(run.run.front());
  // ASSERT(std::all_of(run.run.begin(), run.run.end(), [&](auto c) { return dp::GlyphManager::GetFontIndex(c) == hbFont; }),
  //     ("Not all characters of the string", run.run, "are using the same font"));


}

// void ShapeRunWithFont(std::u16string_view const & text, int runOffset, int runLength, UScriptCode script, bool isRtl,
//                       int8_t lang, TextRunHarfBuzz::ShapeOutput* out) {
/*
void ShapeRunWithFont(FontParams const & fontParams, TextRun & run)
{
  // hb_font_t* harfbuzz_font = CreateHarfBuzzFont(in.skia_face, SkIntToScalar(in.font_size),
  //                        in.render_params, in.subpixel_rendering_suppressed);

  // Create a HarfBuzz buffer and add the string to be shaped. The HarfBuzz
  // buffer holds our text, run information to be used by the shaping engine,
  // and the resulting glyph data.
  hb_buffer_t * buffer = hb_buffer_create();
  // Note that the value of the |item_offset| argument (here specified as
  // |in.range.start()|) does affect the result, so we will have to adjust
  // the computed offsets.
  hb_buffer_add_utf16(buffer, reinterpret_cast<uint16_t const *>(text.data()), static_cast<int>(text.size()), runOffset, runLength);
  hb_buffer_set_script(buffer, ICUScriptToHarfbuzzScript(script));
  hb_buffer_set_direction(buffer, isRtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);

  hb_buffer_set_language(buffer, OrganicMapsLanguageToHarfbuzzLanguage(lang));

  // Shape the text.
  hb_shape(harfbuzz_font, buffer, nullptr, 0);

  // Populate the run fields with the resulting glyph data in the buffer.
  unsigned int glyph_count = 0;
  hb_glyph_info_t * infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
  out->glyph_count = glyph_count;
  hb_glyph_position_t const * hb_positions = hb_buffer_get_glyph_positions(buffer, nullptr);
  out->glyphs.resize(out->glyph_count);
  out->glyph_to_char.resize(out->glyph_count);
  out->positions.resize(out->glyph_count);
  out->width = 0.0f;

  // Font on MAC like ".SF NS Text" may have a negative x_offset. Positive
  // x_offset are also found on Windows (e.g. "Segoe UI"). It requires tests
  // relying on the behavior of |glyph_width_for_test_| to also be given a zero
  // x_offset, otherwise expectations get thrown off
  // (see: http://crbug.com/1056220).
  const bool force_zero_offset = in.glyph_width_for_test > 0;
  constexpr uint16_t kMissingGlyphId = 0;

  out->missing_glyph_count = 0;
  for (size_t i = 0; i < out->glyph_count; ++i) {
    // Max 65535 glyphs in font.
    DCHECK_LE(infos[i].codepoint, std::numeric_limits<uint16_t>::max());
    uint16_t glyph = static_cast<uint16_t>(infos[i].codepoint);
    out->glyphs[i] = glyph;
    if (glyph == kMissingGlyphId)
      out->missing_glyph_count += 1;
    //DCHECK_GE(infos[i].cluster, in.range.start());
    //out->glyph_to_char[i] = infos[i].cluster - in.range.start();
    const float x_offset =
        force_zero_offset ? 0
                          : HarfBuzzUnitsToFloat(hb_positions[i].x_offset);
    const float y_offset =
        HarfBuzzUnitsToFloat(hb_positions[i].y_offset);
    out->positions[i].set(out->width + x_offset, -y_offset);

    if (in.glyph_width_for_test == 0)
      out->width += HarfBuzzUnitsToFloat(hb_positions[i].x_advance);
    else if (hb_positions[i].x_advance)  // Leave zero-width glyphs alone.
      out->width += in.glyph_width_for_test;

    if (in.obscured)
      out->width += in.obscured_glyph_spacing;

    // When subpixel positioning is not enabled, glyph width is rounded to avoid
    // fractional width. Disable this conversion when a glyph width is provided
    // for testing. Using an integral glyph width has the same behavior as
    // disabling the subpixel positioning.
    const bool force_subpixel_for_test = in.glyph_width_for_test != 0;

    // Round run widths if subpixel positioning is off to match native behavior.
    if (!in.render_params.subpixel_positioning && !force_subpixel_for_test)
      out->width = std::round(out->width);
  }

  hb_buffer_destroy(buffer);
  hb_font_destroy(harfbuzz_font);
}
*/


/*
void ShapeRuns(FontParams const & fontParams, TextRuns& outRuns)
{
  for (auto & run : outRuns.runs)
  {
    // TODO(AB): Cache runs.

    ShapeRunWithFont(fontParams, run)

    internal::TextRunHarfBuzz::ShapeOutput output;
    ShapeRunWithFont(cache_key, &output);
    run->UpdateFontParamsAndShape(font_params, output);
    if (can_use_cache)
      cache.get()->Put(cache_key, output);
    //    }

    // Check to see if we still have missing glyphs.
    if (run->shape.missing_glyph_count)
      runs_with_missing_glyphs.push_back(run);
  }
  in_out_runs->swap(runs_with_missing_glyphs);
}*/

TextRuns ItemizeText(std::string_view utf8)
{
  ASSERT(!utf8.empty(), ("Shaping of empty strings is not supported"));
  ASSERT(std::string::npos == utf8.find_first_of("\r\n"), ("Shaping with line breaks is not supported", utf8));

  // TODO(AB): Can unnecessary conversion/allocation be avoided?
  TextRuns textRuns {strings::ToUtf16(utf8), {}};
  GetSingleTextLineRuns(textRuns);
  return textRuns;
}

void ReorderRTL(TextRuns & runs)
{
  // TODO(AB): Optimize implementation to use indexes to runs instead of copying runs.
  auto it = runs.substrings.begin();
  auto const end = runs.substrings.end();
  // TODO(AB): Line (default rendering) direction is determined by the first run. It should be defined as a parameter depending on the language.
  auto const lineDirection = it->m_direction;
  while(it != end)
  {
    if (it->m_direction == lineDirection)
      ++it;
    else
    {
      auto start = it++;
      while (it != end && it->m_direction != lineDirection)
        ++it;
      std::reverse(start, it);
    }
  }
  if (lineDirection != HB_DIRECTION_LTR)
    std::reverse(runs.substrings.begin(), end);
}

TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, int8_t lang)
{
  //ShapeRuns(fontParams, textRuns);
  // TODO
  return TextMetrics{};
}

TextMetrics ShapeText(std::string_view utf8, int fontPixelHeight, char const * lang)
{
  return ShapeText(utf8, fontPixelHeight, StringUtf8Multilang::GetLangIndex(lang));
}

}  // namespace text_shape