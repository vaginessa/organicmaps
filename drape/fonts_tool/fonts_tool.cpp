#include "drape/harfbuzz_shape.hpp"

#include "platform/platform.hpp"

#include "base/file_name_utils.hpp"
#include "base/scope_guard.hpp"
#include "base/string_utils.hpp"

#include <fstream>
#include <iostream>
#include <numeric>  // std::accumulate

#include <ft2build.h>
#include FT_FREETYPE_H

void ItemizeLine(std::string str)
{
  strings::Trim(str);
  if (str.empty())
    return;

  auto const segments = text_shape::ItemizeText(str);
  std::cout << str << " (runs=" << segments.substrings.size() << ")" << "\n";
  for (const auto & run : segments.substrings)
    std::cout << DebugPrint(std::u16string_view{segments.text.data() + run.m_start, static_cast<size_t>(run.m_length)}) << '|';
  std::cout << "\b \b\n";
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " [text file with utf8 strings or any arbitrary text string]\n";
    return -1;
  }

  // Platform::FilesList ttfFiles;
  // GetPlatform().GetFontNames(ttfFiles);
  //
  // auto reader = GetPlatform().GetReader("00_NotoNaskhArabic-Regular.ttf");
  // auto fontFile = reader->GetName();
  //
  // Initialize Freetype.
  // FT_Library library;
  // if (auto const err = FT_Init_FreeType(&library); err != 0)
  // {
  //   std::cerr << "FT_Init_FreeType returned " << err << " error\n";
  //   return 1;
  // }
  // SCOPE_GUARD(doneFreetype, [&library]()
  //             {
  //               if (auto const err = FT_Done_FreeType(library); err != 0)
  //                 std::cerr << "FT_Done_FreeType returned " << err << " error\n";
  //             });

  // Scan all fonts.
  // std::vector<dp::Font> fonts;
  // for (auto const & ttf : ttfFiles)
  // {
  //   std::cout << ttf << "\n";
  //   fonts.emplace_back(4, GetPlatform().GetReader(base::JoinPath(kFontsDir, ttf)), library);
  //   std::vector<FT_ULong> charcodes;
  //   fonts.back().GetCharcodes(charcodes);
  // }

/////////////////////////
  if (Platform::IsFileExistsByFullPath(argv[1]))
  {
    std::ifstream file(argv[2]);
    std::string line;
    while (file.good())
    {
      std::getline(file, line);
      ItemizeLine(line);
    }
  }
  else
  {
    // Get all args as one string.
    std::vector<std::string> const args(argv + 1, argv + argc);
    auto const line = std::accumulate(args.begin(), args.end(), std::string{});
    ItemizeLine(line);
  }
  return 0;
}
