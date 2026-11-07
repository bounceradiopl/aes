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
//    IcecastClient, which has a thread of its own for the network;
//  - a station with Stereo Tool processing (Bounce Encoder 1.1) copies its mix into the queue of
//    its processing worker instead (two threads: output and Stereo Tool), which hands the result
//    to the station's streams. A station without processing has no worker: its path is the 1.0
//    one, bit for bit.
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
#include "bam/encoder/preset_store.hpp"
#include "bam/encoder/stereotool.hpp"
#include "bam/ptp_slave.hpp"
#include "bam/rx_stream.hpp"

namespace bam::encoder {

struct EncoderEngineOptions {
  bool enable_ptp = true;  // false: no PTP sockets, the media clock free-runs (tests)
  // Tests: shifts the free-running media clock off the local time (a PTP lock must move it back).
  int64_t free_run_offset_ns = 0;
  // Stereo Tool's folder (preset_store.hpp): presets\ with the uploaded .sts files and a home folder per
  // station. The service uses "<config folder>\stereotool"; empty: no presets (a station with
  // processing then reports an error and runs unprocessed).
  std::string stereotool_dir;
  bool protect_stereotool_dir = false;  // create its folders with the admin-only ACL (data_dir.hpp)
  // Ruling S7: stop() comes right before the process ends (the service), so it leaves the Stereo
  // Tool instances to the end of the process instead of deleting them: stereoTool_Delete takes
  // about 0.5-1.2 s per instance, one after another, which with many stations would outlast the
  // Windows stop wait. A station removed or switched off while running is still deleted.
  bool keep_stereotool_at_stop = false;
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
  uint64_t receiver_id = 0;  // changes only when the input's socket and RxStream are rebuilt
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

// Stereo Tool processing of a station (processing_worker.cpp). Whatever the state, the station's
// streams get audio: Bypass and Error carry the unprocessed mix (never silence).
enum class ProcessingState {
  Off,      // no processing (not configured, or stopped)
  Running,  // the streams get the Stereo Tool output
  Bypass,   // processing is set up but bypassed: starting, licence, too slow, Stereo Tool failed
  Error,    // processing could not be set up: library, instance, preset, or a crash (see reason)
};
inline const char* processing_state_name(ProcessingState s) {
  switch (s) {
    case ProcessingState::Running: return "running";
    case ProcessingState::Bypass: return "bypass";
    case ProcessingState::Error: return "error";
    case ProcessingState::Off: break;
  }
  return "off";
}

// Why processing is not Running (ProcessingStatus::reason_code).
enum class ProcessingReason {
  None,          // running, or off
  Starting,      // Stereo Tool is being set up (also after a restart or a preset change)
  LicenceCheck,  // set up; the first licence check is still to come
  Setup,         // the library, the instance or the preset failed
  Licence,       // the licence does not cover the features the settings use
  TooSlow,       // Stereo Tool fell behind (retried automatically by the engine)
  Process,       // Stereo Tool failed while processing
  Crash,         // a crash in the library: nothing until the service restarts
  RestartWaits,  // a restart waits for a Stereo Tool call to return
  Saving,        // a preset save took longer than 200 ms: unprocessed until it is over, then back
};
inline const char* processing_reason_name(ProcessingReason r) {
  switch (r) {
    case ProcessingReason::Starting: return "starting";
    case ProcessingReason::LicenceCheck: return "licence_check";
    case ProcessingReason::Setup: return "setup";
    case ProcessingReason::Licence: return "licence";
    case ProcessingReason::TooSlow: return "too_slow";
    case ProcessingReason::Process: return "process";
    case ProcessingReason::Crash: return "crash";
    case ProcessingReason::RestartWaits: return "restart_waits";
    case ProcessingReason::Saving: return "saving";
    case ProcessingReason::None: break;
  }
  return "";
}

struct ProcessingStatus {
  ProcessingState state = ProcessingState::Off;
  ProcessingReason reason_code = ProcessingReason::None;
  std::string reason;     // Polish, why not Running; empty while running
  double latency_ms = 0;  // delay of the processed path (Stereo Tool latency + 5 ms crossfade margin)
  double cpu_pct = 0;     // Stereo Tool time / audio time over the last second processed; 0 when not processing
  bool license_valid = false;        // the licence covers the features the settings use (last check)
  std::string unlicensed_features;   // Stereo Tool's list when the licence check failed
  uint64_t queue_dropped_frames = 0; // 48 kHz frames lost: the worker fell more than 2 s behind the station
  // The Stereo Tool web interface of the station's instance, on 127.0.0.1 only, while the instance
  // is up with it (also in the bypass, to fix the settings); 0: none. An instance abandoned after a
  // crash keeps its port until the service restarts: then 0 and the crash reason.
  int web_port = 0;
  // engine
  bool enabled = false;  // processing is switched on for the station (configuration)
  std::string preset;    // the preset configured
  int retries = 0;       // automatic retries after "too slow" since the last good 10 minutes
  int64_t retry_in_s = -1;  // seconds to the next automatic retry; -1 when none is due
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
  ProcessingStatus processing;  // state Off when the station has no processing running
};

struct EncoderStatus {
  bool running = false;
  std::string interface_ip;
  bool ptp_enabled = true;
  // The media clock does not follow a PTP master right now: PTP is off or unavailable, no master
  // was found yet, it is still being acquired, or it was lost (holdover coasts on the last rate).
  bool ptp_free_run = true;
  std::string ptp_error;     // Polish: PTP sockets unavailable, else empty
  PtpStatus ptp;
  int64_t uptime_s = 0;
  std::vector<StationStatus> stations;  // in configuration order
  // The Stereo Tool library (stereotool_peek: not looked for while no station used processing).
  StereoToolInfo stereotool;
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
  // running when only its streams or input settings change; an edited stream restarts alone,
  // its old connection closed before the new one opens (Icecast refuses a busy mountpoint).
  // Inputs are matched by SDP and interface, not by position: an input keeps its socket and
  // delay line (with its history) while one with the same SDP stays in the station, wherever
  // it moves, through gain, delay and receive latency edits; an input that goes (or gets another
  // SDP) fades out over 5 ms and the new one fades in with an empty delay line. Gain and delay
  // changes are applied live, ramped. Before start() it only stores the configuration.
  //
  // Processing (spec §3): a running station with processing switched on gets a processing worker
  // with the effective key (the station's or the default one), the preset from the presets
  // folder, its home folder and the web interface port stereotool_web_port_base + the station's
  // web port slot (ProcessingConfig::web_port_slot, never renumbered: deleting or moving other
  // stations never moves it). Switching processing on is seamless (the worker
  // starts unprocessed with no delay; when Stereo Tool is ready the output pauses once for its
  // latency and crossfades in). A new preset is loaded into the running instance (crossfade to
  // the unprocessed signal and back, aligned). A new key, port or home folder builds a new
  // instance; switching processing off or a new instance fades the old worker's output out over
  // 5 ms and the station's output (direct, or into the new worker) in over 5 ms, like a delay
  // change. A Stereo Tool too slow is retried automatically after 30 s, 2, 5 and 10 minutes (then
  // every 10 minutes; the count starts again after 10 minutes of processing); every retry and its
  // outcome is logged, like every change of the processing state.
  void apply_config(const EncoderConfig& config);

  // Stereo Tool presets (preset_store.hpp) in EncoderEngineOptions::stereotool_dir. Names compare
  // without regard to case everywhere.
  std::vector<PresetInfo> list_presets() const;
  PresetResult put_preset(std::string_view name, std::string_view data);
  PresetResult get_preset(std::string_view name, std::string* data) const;
  // Refused (InUse) while a station of the configuration refers to it, running or not.
  PresetResult remove_preset(std::string_view name);
  // Saves the station's current Stereo Tool settings (e.g. tuned in its web interface) as a new
  // preset `name`; done by the station's Stereo Tool thread, waited for up to 10 s.
  PresetResult save_processing_preset(const std::string& station_id, std::string_view name);
  // The manual "Uruchom ponownie processing": leaves the bypass and tries again (a failed instance
  // is built anew; after a crash only a service restart helps). false with a Polish *error when
  // the station runs no processing.
  bool restart_processing(const std::string& station_id, std::string* error);

  EncoderConfig config() const;
  EncoderStatus status() const;
  // The media clock the inputs are placed on (PTP, or the local clock while free-running).
  MediaClock media_clock() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bam::encoder
