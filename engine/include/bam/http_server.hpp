// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bam {

class Engine;

struct PanelAsset {
  const char* path;  // "/index.html"
  const char* mime;
  const unsigned char* data;
  size_t size;
};
extern const PanelAsset kPanelAssets[];
extern const size_t kPanelAssetCount;

// REST API + SSE live feed + embedded web panel (Polish UI, Axia-style layout).
class HttpServer {
 public:
  explicit HttpServer(Engine& engine);
  ~HttpServer();
  bool start(const std::string& bind, uint16_t port, std::string* error);
  void stop();

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace bam
