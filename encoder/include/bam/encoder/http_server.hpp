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
//
// Stereo Tool processing (Bounce Encoder 1.1, every route with the session, JSON only): GET
// /api/stereotool (library, presets, web interfaces, warnings), GET /api/stereotool/presets,
// POST /api/stereotool/presets ({name, data_base64}), GET and DELETE
// /api/stereotool/presets/<name>, POST /api/stations/<id>/processing/save-preset ({name}) and
// /processing/restart; /api/status has each station's "processing" and the library's
// "stereotool". Licence keys never leave the server ("license_key_set",
// "stereotool_default_key_set" only). A station update without "processing" keeps the stored
// block (a partial one changes only its keys); switching processing on for an enabled station with
// a preset that is not uploaded is refused (400, field-mapped). Accepted changes answer with
// "warnings" (config_warnings).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
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
// POST /api/stereotool/presets alone, and only with a valid session: base64 of a 4 MB preset
// (kMaxPresetBytes) in its JSON. Every other request (and an upload without a session) keeps
// kMaxApiBodyBytes; one request per connection holds for this route too.
inline constexpr size_t kMaxPresetUploadBytes = 6 * 1024 * 1024;
inline constexpr const char* kPresetUploadPath = "/api/stereotool/presets";

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
//  - wrong Stereo Tool processing settings never stop a station (never silence because of
//    processing): its processing is switched off, a preset name or key of the wrong shape is
//    dropped, the station runs unprocessed; a bad default key is dropped; before a key is
//    dropped the file is kept as "<path>.bad" and the drop is logged (never the key itself);
//    a bad web port base goes back to 8100 (8200 when the panel port is among 8100–8115); a
//    duplicate or out-of-range web port slot is replaced by the lowest free one (logged), the
//    other stations keep theirs.
//  - station ids that differ only in case (Bounce Encoder 1.0 allowed them): every station after
//    the first gets a new id (normalize_config, logged), nothing is disabled;
//  - the warnings of config_warnings (the same Stereo Tool key on two stations) are logged.
EncoderConfig load_startup_config(const std::string& path);

// The panel address for the tray, which may not read config.json (administrators and the service
// only): writes {"bind": "<address>", "port": <n>} to `path` (panel_file_path() in paths.hpp:
// public\panel.json next to config.json) atomically (a temporary file, then a rename over it).
// The parent folder is created when missing (one level). Nothing secret is in it.
bool write_panel_file(const std::string& path, const std::string& bind, int port, std::string* error);

namespace testing {
// Test hook: shifts the clock every EncoderHttpServer uses for sessions and login throttling.
void set_http_clock_offset_ns(int64_t offset_ns);
// Test hook: runs inside a login after the password was verified, before the session is created
// (empty = none).
void set_login_verified_hook(std::function<void()> hook);
}  // namespace testing

}  // namespace bam::encoder
