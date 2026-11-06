// SPDX-License-Identifier: GPL-3.0-or-later
// The look shared by both web panels: panel-common/common.css is embedded into the BounceAES Master
// and the Bounce Encoder asset sets (cmake/embed_files.cmake), byte for byte the source file, and each
// index.html loads it before the panel's own style.css.
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "bam/http_server.hpp"
#ifdef BAM_WITH_CODECS
#include "bam/encoder/http_server.hpp"
#endif

namespace {

std::string read_source(const std::string& relative) {
  std::ifstream f(std::string(BAM_TEST_DATA) + "/../../" + relative, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// The asset `path` of a table, as a string ("" when the table has no such file).
template <typename Asset>
std::string asset(const Asset* table, size_t count, const std::string& path, std::string* mime = nullptr) {
  for (size_t i = 0; i < count; ++i)
    if (path == table[i].path) {
      if (mime) *mime = table[i].mime;
      return std::string(reinterpret_cast<const char*>(table[i].data), table[i].size);
    }
  return "";
}

// common.css is linked, and before style.css (the panel's own rules override the shared ones).
void check_links(const std::string& html) {
  const size_t common = html.find(R"(<link rel="stylesheet" href="common.css">)");
  const size_t own = html.find(R"(<link rel="stylesheet" href="style.css">)");
  REQUIRE(common != std::string::npos);
  REQUIRE(own != std::string::npos);
  CHECK(common < own);
}

}  // namespace

TEST_CASE("panel assets: both panels carry the shared stylesheet") {
  const std::string source = read_source("panel-common/common.css");
  REQUIRE(source.size() > 1000);
  CHECK(source.find("--brand: #f72868") != std::string::npos);

  std::string mime;
  const std::string master = asset(bam::kPanelAssets, bam::kPanelAssetCount, "/common.css", &mime);
  CHECK(master == source);
  CHECK(mime == "text/css; charset=utf-8");
  CHECK_FALSE(asset(bam::kPanelAssets, bam::kPanelAssetCount, "/style.css").empty());  // its own rules stay
  check_links(asset(bam::kPanelAssets, bam::kPanelAssetCount, "/index.html"));

#ifdef BAM_WITH_CODECS
  namespace be = bam::encoder;
  const std::string encoder = asset(be::kEncoderPanelAssets, be::kEncoderPanelAssetCount, "/common.css", &mime);
  CHECK(encoder == source);
  CHECK(mime == "text/css; charset=utf-8");
  CHECK_FALSE(asset(be::kEncoderPanelAssets, be::kEncoderPanelAssetCount, "/style.css").empty());
  CHECK(asset(be::kEncoderPanelAssets, be::kEncoderPanelAssetCount, "/style.css") !=
        asset(bam::kPanelAssets, bam::kPanelAssetCount, "/style.css"));
  check_links(asset(be::kEncoderPanelAssets, be::kEncoderPanelAssetCount, "/index.html"));
#endif
}
