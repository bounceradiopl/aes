// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder REST API and embedded web panel (spec §7, §8), on the pattern of the BounceAES
// Master panel server: cpp-httplib, panel login with a session cookie (bam/auth.hpp), the panel
// files compiled in (cmake/embed_files.cmake, encoder/panel/).
//
// Every configuration change goes config_from_json → merge_passwords → normalize_config →
// validate_config; an invalid one is refused with 400 and the Polish issues, and nothing changes.
// An accepted one is saved to the configuration file first, then applied to the engine; changes
// are serialized (one at a time). Stream passwords and the panel password hash never leave the
// server (config_to_api_json).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "bam/encoder/config.hpp"

namespace bam::encoder {

class EncoderEngine;

struct EncoderPanelAsset {
  const char* path;  // "/index.html"
  const char* mime;
  const unsigned char* data;
  size_t size;
};
extern const EncoderPanelAsset kEncoderPanelAssets[];
extern const size_t kEncoderPanelAssetCount;

inline constexpr size_t kMaxApiBodyBytes = 1024 * 1024;  // larger request bodies get 413

class EncoderHttpServer {
 public:
  // `config_path`: the file every accepted change is saved to (the service's config.json).
  EncoderHttpServer(EncoderEngine& engine, std::string config_path);
  ~EncoderHttpServer();  // stop()
  EncoderHttpServer(const EncoderHttpServer&) = delete;
  EncoderHttpServer& operator=(const EncoderHttpServer&) = delete;

  // Port 0 picks a free port (tests); port() tells which. false with a Polish *error when the
  // address cannot be bound.
  bool start(const std::string& bind, uint16_t port, std::string* error);
  void stop();
  uint16_t port() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// The configuration the service starts with, from `path`, always normalized and valid:
//  - no file: the defaults, written to `path`;
//  - a file that cannot be read as a configuration: renamed to "<path>.bad", the defaults used
//    and written;
//  - a readable file with errors (edited by hand): each issue is logged, a global setting with an
//    error goes back to its default, a station with an error is disabled (the panel shows it, the
//    user fixes it there); a station that is still invalid while disabled is left out, after a
//    copy of the file is kept as "<path>.bad". The file itself is only rewritten by the next
//    change from the panel.
EncoderConfig load_startup_config(const std::string& path);

}  // namespace bam::encoder
