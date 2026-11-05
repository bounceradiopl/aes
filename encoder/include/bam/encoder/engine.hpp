// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine (spec §3, §4): AES67 inputs on the PTP media clock, one thread per
// station (delay lines + mixer) and one worker per stream (gain, resampling, encoder, Icecast).
//
// Threads and handovers:
//  - one network thread receives every input (an RxStream per input writing its own ring pair
//    in process memory, 65536 frames);
//  - the PTP thread runs a slave-only PtpPort (domain from the configuration); without a master
//    the media clock free-runs on the local clock;
//  - each enabled station runs a thread waking every 10 ms: it reads each input ring up to
//    "now - receive latency", feeds the input's delay line, mixes, and copies the mix into the
//    bounded queue (5 s, oldest dropped and counted) of each of its streams;
//  - each enabled stream runs a worker thread (stream gain + resampling, encoder) feeding its
//    IcecastClient, which has a thread of its own for the network.
//  Configuration changes are built on the calling thread (sockets, rings, 23 MB delay lines,
//  encoders) and handed to the station thread as an immutable plan; only what changed is
//  rebuilt, so editing one stream leaves the station and its other streams running.
//
// The EncoderEngine methods may be called from any thread; they are serialized internally and
// never run inside the audio threads.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bam/clock.hpp"
#include "bam/encoder/config.hpp"
#include "bam/encoder/icecast.hpp"
#include "bam/ptp_slave.hpp"
#include "bam/rx_stream.hpp"

namespace bam::encoder {

struct EncoderEngineOptions {
  bool enable_ptp = true;  // false: no PTP sockets, the media clock free-runs (tests)
  // Tests: shifts the free-running media clock off the local time (a PTP lock must move it back).
  int64_t free_run_offset_ns = 0;
};

struct InputStatus {
  int index = 0;  // position in the station's inputs (0-based)
  bool enabled = false;
  std::string name;
  std::string address;  // "group:port" from the SDP
  bool signal = false;  // packets within the last second
  std::string error;    // Polish: why reception is not active (bad SDP, socket), else empty
  uint32_t latency_us = 0;
  double gain_db = 0;
  double delay_s = 0;
  RxStats rx;
  float peak_db[2] = {-120, -120};  // before the input gain, over the last ~100 ms
};

struct StreamStatus {
  std::string id;
  bool enabled = false;
  Codec codec = Codec::Mp3;
  uint32_t bitrate_kbps = 0;
  uint32_t sample_rate = 0;
  std::string host;
  int port = 0;
  std::string mount;
  bool codec_available = false;
  std::string codec_library;  // the run-time library loaded for the codec; empty for FLAC (built in) or when missing
  IcecastStatus icecast;
  std::string encoder_error;  // Polish: the codec is missing, refused the settings or failed; empty when working
  uint64_t queue_dropped_frames = 0;        // 48 kHz frames lost: the stream fell more than 5 s behind
  uint64_t conditioner_dropped_frames = 0;  // 48 kHz frames the resampler lost
  float peak_db[2] = {-120, -120};          // after the stream gain, over the last ~100 ms
  int64_t started_ns = 0;  // monotonic time the stream worker was built: changes only when the stream restarts
};

struct StationStatus {
  std::string id;
  std::string name;
  bool enabled = false;
  bool running = false;
  int64_t started_ns = 0;  // monotonic time the station thread started
  float peak_db[2] = {-120, -120};  // the mix, over the last ~100 ms
  uint64_t skipped_frames = 0;      // frames the station could not process (stall over 1 s, clock step)
  std::vector<InputStatus> inputs;
  std::vector<StreamStatus> streams;
};

struct EncoderStatus {
  bool running = false;
  std::string interface_ip;
  bool ptp_enabled = true;
  bool ptp_free_run = true;  // no PTP time yet: running on the local clock
  std::string ptp_error;     // Polish: PTP sockets unavailable, else empty
  PtpStatus ptp;
  int64_t uptime_s = 0;
  std::vector<StationStatus> stations;  // in configuration order
};

class EncoderEngine {
 public:
  explicit EncoderEngine(EncoderEngineOptions opts = {});
  ~EncoderEngine();  // stop()
  EncoderEngine(const EncoderEngine&) = delete;
  EncoderEngine& operator=(const EncoderEngine&) = delete;

  // Starts the network and PTP threads and every enabled station of `config`, which must be
  // normalized and valid (normalize_config + validate_config). false (Polish *error) when
  // already running.
  bool start(const EncoderConfig& config, std::string* error);
  // Stops every thread and closes every connection; idempotent.
  void stop();
  // Applies a normalized, valid configuration (merge_passwords, normalize_config and
  // validate_config already done) while running, rebuilding only what changed: a station keeps
  // running when only its streams or input settings change, an edited stream restarts alone, an
  // input keeps its socket unless its SDP, latency or the interface changed. Gain and delay
  // changes are applied without a restart. Before start() it only stores the configuration.
  void apply_config(const EncoderConfig& config);

  EncoderConfig config() const;
  EncoderStatus status() const;
  // The media clock the inputs are placed on (PTP, or the local clock while free-running).
  MediaClock media_clock() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bam::encoder
