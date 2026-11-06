// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder configuration (spec §7): the JSON file %ProgramData%\Bounce Encoder\config.json
// ("version": 1), its validation against the limits of spec §2 and the codec table of §5.2,
// normalisation, and the password handling of the API (stream passwords are never returned;
// an empty password in an update keeps the stored one).
//
// Identifiers: every station has an id unique in the configuration ("s1", "s2", ...) and every
// stream an id unique in its station ("str1", "str2", ...). normalize_config() gives the smallest
// free number to each station / stream whose id is empty (a new one from the panel); ids are
// otherwise kept as they are, so a stream keeps its id when another stream of the station is
// deleted. Ids are 1–32 characters of A–Z, a–z, 0–9, '-' and '_' (they appear in API paths).
//
// An update from the API goes: config_from_json → merge_passwords(update, current) →
// normalize_config → validate_config. merge_passwords must run before normalize_config, so a
// stream added in the same update (id still empty) never inherits another stream's password.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bam/encoder/audio_encoder.hpp"

namespace bam::encoder {

inline constexpr int kEncoderConfigVersion = 1;
inline constexpr int kMaxStations = 16;
inline constexpr int kMaxInputsPerStation = 3;
inline constexpr int kMaxStreamsPerStation = 8;
inline constexpr double kGainMinDb = -60.0;  // inputs and streams, 0.5 dB steps
inline constexpr double kGainMaxDb = 12.0;
inline constexpr double kMaxDelaySeconds = 60.0;  // input delay, 0.1 s steps
inline constexpr int kDefaultEncoderHttpPort = 8070;

// One AES67 input of a station.
struct InputConfig {
  bool enabled = true;
  std::string name;  // empty = the SDP session name (filled in by normalize_config)
  std::string sdp;
  double gain_db = 0;
  double delay_s = 0;
  uint32_t latency_us = 5000;  // receive latency, at least the source ptime + 1 ms
};

// One Icecast stream of a station.
struct StreamConfig {
  std::string id;  // unique in the station; empty = new (normalize_config assigns one)
  bool enabled = true;
  Codec codec = Codec::Mp3;
  uint32_t bitrate_kbps = 128;  // not used for FLAC
  uint32_t sample_rate = 44100;
  double gain_db = 0;
  std::string host;  // host name or IPv4 address
  int port = 8000;
  std::string mount;  // starts with '/'
  std::string user = "source";
  std::string password;
};

struct StationConfig {
  std::string id;  // unique in the configuration; empty = new (normalize_config assigns one)
  std::string name;
  bool enabled = true;
  std::vector<InputConfig> inputs;
  std::vector<StreamConfig> streams;
};

struct EncoderConfig {
  std::string interface_ip;  // empty = first non-loopback IPv4 interface
  int ptp_domain = 0;
  std::string http_bind = "0.0.0.0";
  int http_port = kDefaultEncoderHttpPort;
  std::string password_hash;  // panel login (bam/auth.hpp make_password_hash)
  std::vector<StationConfig> stations;
};

struct EncoderConfigIssue {
  std::string field;    // JSON path, e.g. "stations[0].streams[1].bitrate_kbps"
  std::string message;  // Polish, user facing, names the station / input / stream
};

EncoderConfig default_encoder_config();  // no stations, panel on 0.0.0.0:8070

// The file form (with stream passwords and the panel password hash).
std::string config_to_json(const EncoderConfig& c);
// The API form: like the file, but every stream "password" is "" with "password_set" telling
// whether one is stored, and there is no "password_hash".
std::string config_to_api_json(const EncoderConfig& c);
// Missing keys keep their defaults, unknown keys are ignored. nullopt (with a Polish *error)
// for invalid JSON, a value of the wrong type, an unknown codec, a "version" other than 1, or an
// array longer than 4x its limit (64 stations, 12 inputs or 32 streams per station: an API body
// cannot make normalize_config work through thousands of entries; validate_config explains a
// smaller overshoot with the real limit).
std::optional<EncoderConfig> config_from_json(std::string_view text, std::string* error);

// Every rule of spec §2, §5.2 and §7; empty = valid. Disabled inputs and streams are only
// checked for their ranges (gain, delay, latency) and ids: they may be left half filled in.
// Lengths are bounded everywhere (names 64 bytes, SDP 64 KB, host 253, mount/user/password 255).
// Enabled streams get the encoder's own settings check (validate_encoder_settings), a server
// (ASCII host name or IPv4, no port), port 1–65535, a mountpoint starting with '/' without
// spaces, '?' or '#', and a password (ruling E7); two enabled streams of enabled stations may not
// feed the same server mountpoint.
std::vector<EncoderConfigIssue> validate_config(const EncoderConfig& c);
// Rounds gains to 0.5 dB and delays to 0.1 s, raises input latencies below "source ptime + 1 ms",
// names unnamed inputs after their SDP session (control characters removed, cut to 64 bytes on a
// whole UTF-8 character) and assigns ids to new stations and streams.
// Returns Polish notices for the changes the user should hear about (raised latencies).
std::vector<std::string> normalize_config(EncoderConfig& c);

// A copy without any secret: stream passwords and the panel password hash are emptied.
EncoderConfig mask_passwords(const EncoderConfig& c);
// For an update from the API. Each stream of `next` with an empty password takes the password of
// the stream of `previous` with the same station id and stream id, provided it still goes to the
// same server (host, case-insensitive, and port): a stream moved to another server must be given
// its password again, so the old server's password is never sent to a different one. Streams are
// matched by id, never by position, so deleting a stream keeps the others' passwords. The panel
// password hash always comes from `previous` (it changes only through the password endpoint).
void merge_passwords(EncoderConfig& next, const EncoderConfig& previous);

// Reads the configuration file; *missing = true when it does not exist (then nullopt, no error).
std::optional<EncoderConfig> load_config_file(const std::string& path, std::string* error, bool* missing = nullptr);
// Writes the configuration atomically (a temporary file next to it, flushed to disk, then a rename
// over it), creating the directory when needed. On failure the temporary file is removed.
bool save_config_file(const std::string& path, const EncoderConfig& c, std::string* error);

}  // namespace bam::encoder
