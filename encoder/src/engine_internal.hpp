// SPDX-License-Identifier: GPL-3.0-or-later
// Internal pieces of the Bounce Encoder engine (engine.hpp): inputs, the PTP slave, stations and
// stream workers. Everything here is built on an API thread and handed to the audio threads
// whole; see engine.hpp for the thread layout.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bam/clock.hpp"
#include "bam/encoder/config.hpp"
#include "bam/encoder/dsp.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/icecast.hpp"
#include "bam/platform/udp.hpp"
#include "bam/ptp_slave.hpp"
#include "bam/rx_stream.hpp"
#include "bam/timeline.hpp"

namespace bam::encoder::detail {

// Linear level (1.0 = full scale) -> dBFS, floored at -120.
float level_db(float linear);

// ---- inputs (inputs.cpp) ----

// One received AES67 input: its socket (joined to the SDP group, with the source filter when
// the SDP has one), the RxStream and the ring pair it writes on the media clock. Immutable after
// make_input_rx() except for the RxStream state, which only the network thread touches, and
// latency_frames, which a latency edit changes in place (engine side, with the RxStream's own
// copy set through RxStream::set_latency_frames).
struct InputRx {
  InputRx();
  ~InputRx();
  InputRx(const InputRx&) = delete;
  InputRx& operator=(const InputRx&) = delete;
  const uint64_t id;  // unique per receiver built (status: receiver_id)
  std::string key;  // SDP | interface: the input's identity (another key is another input)
  std::string name;
  std::string group;
  std::string source;  // sender address from a=source-filter, may be empty
  uint16_t port = 0;
  uint32_t latency_frames = 0;  // engine (API/housekeeping) side, under the engine lock; RxStream has its own
  std::string error;  // Polish; set when the SDP or the socket failed (then stream is null)
  bool retryable = false;    // the socket failed (not the SDP): worth another make_input_rx()
  int64_t attempted_ns = 0;  // when the socket was tried
  platform::UdpSocket socket;
  std::array<std::vector<int32_t>, 2> samples;
  std::array<std::vector<uint32_t>, 2> tags;
  std::array<ChannelRing, 2> rings;
  std::unique_ptr<RxStream> stream;
};

std::string input_key(const InputConfig& in, const std::string& iface);
// Parses the SDP, allocates the rings, opens the socket on the SDP port and joins the group.
std::shared_ptr<InputRx> make_input_rx(const InputConfig& in, const std::string& iface);

using RxSet = std::vector<std::shared_ptr<InputRx>>;

// The IPv4 address of the default network card (platform::default_interface_ip(), or the test
// hook); empty when none is up.
std::string default_interface_ip();

// A delay line counted by testing::live_delay_lines().
std::shared_ptr<DelayLine> make_delay_line();
// How long a receiver whose socket failed waits before the next attempt (5 s).
int64_t input_retry_ns();

namespace testing {
int live_input_receivers();  // InputRx objects alive (sockets open or not)
int live_delay_lines();      // delay lines alive (23 MB each)
void fail_next_input_opens(int n);  // the next n socket opens fail (0 = off)
void set_input_retry_ms(int ms);    // retry interval for failed sockets (0 = the default 5 s)
int input_receivers_freed_on_rx_thread();  // InputRx objects destroyed on the network thread
// Replaces platform::default_interface_ip() for the automatic interface (empty = the real one).
void set_default_interface_hook(std::function<std::string()> hook);
}  // namespace testing

// The network thread: receives every input of the current set (re-read every pass), dispatching
// a datagram only to the input whose group (and source, when filtered) it was sent to.
class RxThread {
 public:
  explicit RxThread(const ClockSource& clock);
  ~RxThread();  // stop()
  // Returns the set it replaces. The network thread may still be using that set: the caller
  // keeps it until it holds the only reference, so its sockets and rings are never released on
  // the network thread.
  std::shared_ptr<const RxSet> set_inputs(std::shared_ptr<const RxSet> inputs);
  void stop();

 private:
  void run();
  std::shared_ptr<const RxSet> snapshot() const;

  const ClockSource& clock_;
  mutable std::mutex m_;
  std::shared_ptr<const RxSet> inputs_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

// ---- PTP slave (ptp_client.cpp) ----

// Slave-only PTP on the configured domain and interface; steers `clock` once it follows a
// master. Without a master the clock is left as it is (free-running on the local clock).
class PtpClient {
 public:
  PtpClient(ClockSource& clock, int domain, std::string iface);
  ~PtpClient();  // stop()
  void stop();
  PtpStatus status() const;
  std::string error() const;
  int domain() const { return domain_; }
  const std::string& iface() const { return iface_; }

 private:
  void run();
  ClockSource& clock_;
  const int domain_;
  const std::string iface_;
  ClockIdentity self_;
  mutable std::mutex m_;
  PtpStatus status_;
  std::string error_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

// ---- stream workers (stream_worker.cpp) ----

// The bounded handover from a station thread to one stream worker: interleaved float stereo
// frames, at most `capacity` of them; a push that does not fit drops the oldest frames and
// counts them. Both sides hold the lock only to copy.
class AudioQueue {
 public:
  explicit AudioQueue(size_t capacity_frames);
  void push(const float* stereo, size_t frames);
  // Waits up to wait_ms for frames, then takes up to max_frames; 0 when none (or woken).
  size_t pop(float* out, size_t max_frames, int wait_ms);
  void wake();  // makes a waiting pop() return
  size_t size_frames() const;  // frames waiting now
  uint64_t dropped_frames() const { return dropped_.load(std::memory_order_relaxed); }
  size_t capacity_frames() const { return capacity_; }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::vector<float> ring_;
  const size_t capacity_;
  size_t head_ = 0;  // oldest frame
  size_t size_ = 0;
  bool woken_ = false;
  std::atomic<uint64_t> dropped_{0};
};

std::string stream_key(const StreamConfig& s, const std::string& station_name);
// Icecast queue limit: about 5 s of the stream, at least 64 KB (ruling E6).
size_t icecast_queue_limit(const StreamConfig& s);

// One enabled stream: its queue from the station, the thread running gain + resampling and the
// encoder, and the Icecast client. Built (and its encoder opened) on the calling thread.
class StreamWorker {
 public:
  static constexpr size_t kQueueFrames = size_t(kStationRate) * 5;  // 5 s
  StreamWorker(const StreamConfig& cfg, const std::string& station_name);
  ~StreamWorker();  // stop()
  StreamWorker(const StreamWorker&) = delete;
  StreamWorker& operator=(const StreamWorker&) = delete;

  const std::string& key() const { return key_; }
  const StreamConfig& config() const { return cfg_; }
  // False when the encoder could not be made or opened (status() says why): nothing runs, and
  // the station does not feed it.
  bool active() const { return active_; }
  // Station thread: hands over a block of the mix (never waits on the encoder or the network).
  void push(const float* stereo, size_t frames) { queue_.push(stereo, frames); }
  // Any thread: the new gain is applied by the worker thread (ramped).
  void set_gain_db(double db);
  double gain_db() const { return gain_db_.load(); }  // the gain set last
  void stop();
  StreamStatus status() const;

 private:
  void run();
  bool reopen();  // worker thread: (re)opens the encoder; false with the error recorded
  void set_error(std::string e);

  StreamConfig cfg_;  // as built (gain updated through set_gain_db)
  const std::string key_;
  const int64_t started_ns_;
  AudioQueue queue_;
  std::unique_ptr<AudioEncoder> encoder_;
  std::unique_ptr<StreamConditioner> conditioner_;
  std::unique_ptr<IcecastClient> client_;
  std::atomic<bool> restart_{false};  // set on the client thread (on_reconnect)
  std::atomic<double> gain_db_;
  std::atomic<uint64_t> conditioner_dropped_{0};
  std::array<std::atomic<float>, 2> peak_{};
  mutable std::mutex error_m_;
  std::string error_;
  bool active_ = false;  // set once by the constructor
  std::atomic<bool> running_{true};
  std::thread thread_;
};

// ---- Stereo Tool processing (processing_worker.cpp) ----

// What a ProcessingWorker runs with, fixed for its lifetime.
struct ProcessingSettings {
  std::string license_key;  // the effective key (the station's, else the default); a secret: never logged
  std::string preset_path;  // the .sts to load; empty: Stereo Tool's own defaults
  std::string home_dir;     // the instance's home folder; empty: the library's default
  int web_port = 0;         // the instance's web interface (127.0.0.1 only); 0: off
  std::string station_name;  // for the log ("Processing stacji „…”"); empty: the station id
  std::string setup_error;  // Polish: why processing cannot be set up at all (nothing is created)
  // The streams the output goes to from the first block on (set_streams() replaces them later):
  // a worker never hands a block to no stream list.
  std::vector<std::shared_ptr<StreamWorker>> streams;
  // Instances to wait for before this one is created (other workers' instance_gone(), being
  // stopped): the ones on the same web interface port, which is one socket. Waited for at most the
  // stop timeout + 1 s (an instance stuck in the library keeps its port: then it is refused,
  // logged); meanwhile the streams get the unprocessed mix, as during any setup.
  std::vector<std::shared_ptr<const std::atomic<bool>>> wait_for_gone;
  // Tests: sees every block handed to the streams, on the output thread (must not allocate).
  std::function<void(const float* stereo, size_t frames)> output_tap;
  // Tests: called on the Stereo Tool thread after each block it processed.
  std::function<void()> st_block_tap;
};

namespace testing {
// How long stop() waits for the Stereo Tool thread before leaving it (0: the default 5 s).
void set_processing_stop_timeout_ms(int ms);
// The engine's automatic retries after "too slow" (ruling S1): the waits before retry 1, 2, 3 and
// every later one, and how long processing must run to start the count again (empty / 0: the
// defaults 30 s, 2, 5, 10 min and 10 min).
void set_processing_retry_ms(std::vector<int> waits_ms, int reset_ms);
// Runs in apply_config right after a processing worker was built, before the station gets its
// plan (e.g. as long as a slow log line); empty: none.
void set_processing_built_hook(std::function<void()> hook);
// How long a preset save is waited for (0: the default 10 s).
void set_save_preset_timeout_ms(int ms);
}  // namespace testing

// How long a preset save is waited for (EncoderEngine::save_processing_preset): 10 s.
int save_preset_timeout_ms();

// The last kSlots values one thread wrote, numbered 1, 2, ... (their seq), read by any thread
// without a lock and without blocking the writer (ProcessingWorker's events).
class EventRing {
 public:
  static constexpr size_t kSlots = 32;
  void push(uint64_t value);  // the one writer thread only
  // Appends (seq, value) of every event after `since` that is still intact, oldest first, and
  // returns the seq to pass next time. An event the writer overwrote meanwhile (more than
  // kSlots - 1 newer ones) is left out: the caller sees the gap in seq. `between` (tests) runs
  // after the slots were read, before they are checked.
  uint64_t read(uint64_t since, std::vector<std::pair<uint64_t, uint64_t>>* out,
                const std::function<void()>& between = {}) const;

 private:
  std::array<std::atomic<uint64_t>, kSlots> slots_{};
  std::atomic<uint64_t> seq_{0};
};

// A change of a worker's state or reason (ProcessingWorker::events_since), for the engine's log.
struct ProcessingEvent {
  uint64_t seq = 0;  // 1, 2, ... per worker
  ProcessingState state = ProcessingState::Off;
  ProcessingReason reason_code = ProcessingReason::None;
  std::string reason;  // Polish, as in ProcessingStatus (empty when running)
};

// Stereo Tool between a station and its streams (spec §3), one per station with processing.
//
// Threads: the station thread only copies its mix into the bounded queue (push). The output
// thread takes it in blocks of 10 ms, keeps the unprocessed signal in a ring (the bypass), hands
// each block to the Stereo Tool thread and waits for it, then crossfades processed and
// unprocessed and hands the result to the streams. The Stereo Tool thread owns the instance: it
// creates it, loads the preset, measures the latency and processes the blocks. Neither audio
// thread allocates per block, and no lock is held across a Stereo Tool call; the output thread
// never waits on Stereo Tool for more than kMaxBehindNs, so a stuck or slow Stereo Tool ends in
// the bypass, not in silence.
//
// Timing: both paths are delayed by the same delay_frames() = Stereo Tool latency + kRampFrames
// (the margin lets every switch away from Stereo Tool crossfade over kRampFrames, even when the
// failed block itself never came back). The delay is 0 until the latency is known and never
// shrinks; when it grows the output pauses for the difference (the streams get the audio later,
// nothing is skipped or inserted). Every switch processed <-> unprocessed is a kRampFrames
// crossfade of the two, aligned.
// For the engine: that pause happens once, when Stereo Tool becomes ready (about 90 ms with the
// real library: 4096 + 240 frames): for that long the streams get nothing at all, then the audio
// goes on where it stopped; no silence is inserted. A restart pauses again only if the latency
// grew. Audio pushed before Stereo Tool is ready comes out unprocessed with no delay.
//
// Bypass (spec §3): until Stereo Tool is ready and its licence was found valid for the settings
// (checked after kLicenceFirstCheckFrames of processed audio, then every kLicenceCheckFrames);
// for good on a licence found invalid or a Stereo Tool failure; and when Stereo Tool cannot keep
// up ("kolejka > 200 ms"): a call out for longer than kMaxBehindNs (stuck), a backlog of more
// than kMaxBehindNs of audio that has not shrunk over kBehindWindowNs (slower than real time; the
// window is long enough for a Stereo Tool at 95 % CPU to show its drain above the 10 ms jitter of
// the queue), or a backlog of kMaxBacklogFrames that has not shrunk over kCapWindowNs (the queue
// would overflow soon). A burst after a station stall that Stereo Tool drains never trips it.
// restart() leaves the bypass. Every change of state or reason is an event (events_since) the
// engine logs; an event keeps the failure text of its moment.
class ProcessingWorker {
 public:
  static constexpr size_t kQueueFrames = size_t(kStationRate) * 2;  // 2 s from the station
  static constexpr size_t kBlockFrames = 480;                       // 10 ms per Stereo Tool call
  static constexpr uint32_t kMaxLatencyFrames = kStationRate / 2;   // a longer latency is refused
  static constexpr int64_t kMaxBehindNs = 200'000'000;
  static constexpr size_t kBehindFrames = size_t(kStationRate) / 5;          // kMaxBehindNs of audio
  static constexpr int64_t kBehindWindowNs = 500'000'000;
  static constexpr size_t kMaxBacklogFrames = size_t(kStationRate) * 3 / 2;  // 1.5 s: bypass before the queue drops
  // A backlog over kMaxBacklogFrames that has not shrunk over this long bypasses (Stereo Tool
  // drains 10 ms more than the queue's jitter in it below about 90 % CPU).
  static constexpr int64_t kCapWindowNs = 100'000'000;
  static constexpr uint64_t kLicenceFirstCheckFrames = kStationRate / 2;
  // Re-checked every 250 ms of audio (cheap in the fake; Task 7 measures the real library's cost).
  static constexpr uint64_t kLicenceCheckFrames = kStationRate / 4;
  static constexpr size_t kEventSlots = EventRing::kSlots;  // events kept for events_since()

  // Starts both threads; the instance is built on the Stereo Tool thread (meanwhile, and on any
  // failure, the streams get the unprocessed mix).
  ProcessingWorker(const ProcessingSettings& s, std::string station_id);
  ~ProcessingWorker();  // stop()
  ProcessingWorker(const ProcessingWorker&) = delete;
  ProcessingWorker& operator=(const ProcessingWorker&) = delete;

  // Station thread: hands over a block of the mix (copies only, never waits on Stereo Tool).
  void push(const float* stereo, size_t frames) { in_.push(stereo, frames); }
  // Any thread: the streams the output goes to, taken by the output thread at its next block;
  // lists it let go of are released here (on the caller's thread), or by stop().
  void set_streams(std::vector<std::shared_ptr<StreamWorker>> streams);
  // Any thread: leaves the bypass and tries again (a failed or missing instance is built anew,
  // a working one is kept with its settings). After a crash nothing is created until the
  // service restarts; while Stereo Tool is stuck in a call, the restart waits for it to return.
  void restart();
  // Any thread: loads another preset into the running instance (a restart that loads it first:
  // crossfade to the unprocessed signal, the preset, the latency, the licence check, crossfade
  // back). A missing or failed instance is built with it.
  void set_preset(std::string sts_path);
  // Any thread: the station's name in the worker's own log lines (after a rename).
  void set_station_name(std::string name);
  // Any thread (not the audio threads): saves the instance's current settings to `sts_path` on the
  // Stereo Tool thread, between two blocks, and waits up to timeout_ms for it. Meanwhile the
  // output waits too; a save longer than kMaxBehindNs bypasses (reason Saving, never "too slow")
  // until it is over, then processing goes on by itself (a new session on the same instance, no
  // restart). false with a Polish *error; *busy is set when another save was under way.
  bool save_preset(const std::string& sts_path, int timeout_ms, std::string* error, bool* busy = nullptr);
  // Any thread: fades the output out over kRampFrames of the next frames emitted, then hands
  // nothing more to the streams (faded_out()). For a switch away from this worker: the station's
  // new path fades in once faded_out() (the engine waits for it, bounded).
  void fade_out() { fade_wanted_.store(true, std::memory_order_relaxed); }
  bool faded_out() const { return faded_out_.load(std::memory_order_acquire); }
  ProcessingStatus status() const;
  // Any thread: the state changes after event `since` (0: all kept), oldest first; returns the
  // seq to pass next time. Only the last kEventSlots - 1 are kept: the caller sees the gap in seq.
  uint64_t events_since(uint64_t since, std::vector<ProcessingEvent>* out) const;
  uint32_t delay_frames() const { return delay_.load(std::memory_order_relaxed); }
  // The web interface port it was built with (ProcessingSettings::web_port).
  int configured_web_port() const;
  // Set once its Stereo Tool thread has let go of the instance for good (after stop(); never when
  // that thread is stuck in the library): ProcessingSettings::wait_for_gone of a later worker.
  std::shared_ptr<const std::atomic<bool>> instance_gone() const;
  // Stops both threads; waits for the Stereo Tool thread at most the stop timeout (a call stuck
  // in the library leaves that thread behind, logged). Idempotent.
  void stop();

  struct Core;  // shared with the Stereo Tool thread, which may outlive the worker (stop())

 private:
  enum class Mode { Dry, Probation, Processed };
  using Reason = ProcessingReason;
  enum class Pending { None, Setup, Job };
  void run();
  void take_streams();
  void begin_restart();
  void resume_session();
  bool preset_pending() const;
  bool take_result(bool wait);    // a finished Stereo Tool command, handled; false when none
  void wait_job();
  void go_dry(Reason why, ProcessingState state);
  void set_state(ProcessingState state, Reason why);  // output thread (and constructor / stop())
  std::string reason_text(Reason why, uint64_t failure_id) const;
  bool falling_behind(int64_t now);
  void emit();
  void deliver(const float* stereo, size_t frames);

  const std::string station_id_;
  AudioQueue in_;
  std::shared_ptr<Core> core_;
  std::function<void(const float*, size_t)> output_tap_;

  std::mutex streams_m_;
  std::shared_ptr<const std::vector<std::shared_ptr<StreamWorker>>> pending_streams_;
  // At most the one list the output thread let go of since the last set_streams() (it takes one
  // pending list at a time); released by set_streams()/stop().
  std::vector<std::shared_ptr<const void>> graveyard_;
  std::atomic<bool> has_pending_streams_{false};

  // output thread only
  std::shared_ptr<const std::vector<std::shared_ptr<StreamWorker>>> streams_;
  std::vector<float> block_, dry_, proc_, out_;  // dry_/proc_: rings indexed by input frame
  int64_t written_ = 0;        // input frames in dry_ so far (the next input frame's index)
  int64_t emitted_ = 0;        // the next input frame to emit
  int64_t proc_end_ = 0;       // proc_ holds processed frames [session_start_, proc_end_)
  int64_t session_start_ = 0;  // first input frame of the current Stereo Tool session
  uint32_t session_latency_ = 0;
  int64_t job_start_ = 0;      // input frame of the block at the Stereo Tool thread
  size_t job_frames_ = 0;
  Mode mode_ = Mode::Dry;
  bool licence_ok_ = false;    // the current session's licence check passed
  Pending pending_ = Pending::None;
  bool restart_wanted_ = false;
  bool reload_wanted_ = false;  // a new preset (set_preset) waits for a restart to load it
  bool resume_after_save_ = false;  // in the bypass for a save: back into processing once it is over
  GainRamp fade_{1.0};          // fade_out(): the output level
  bool fading_ = false;
  bool faded_ = false;
  int64_t job_posted_ns_ = 0;  // when the block at the Stereo Tool thread was handed over
  int64_t window_ns_ = 0;      // start of the backlog window (0: backlog under kBehindFrames)
  size_t window_backlog_ = 0;  // the backlog at window_ns_
  int64_t cap_ns_ = 0;         // when the backlog was last looked at above kMaxBacklogFrames (0: below)
  size_t cap_backlog_ = 0;     // the backlog then
  GainRamp mix_{0.0};          // share of the processed signal
  uint64_t failure_id_ = 0;    // the failure text of the last failed Stereo Tool command (Core)

  std::atomic<uint32_t> delay_{0};
  // state << 56 | reason << 48 | failure id: read together; the failure id names the text the
  // state was set with (Core keeps the last few), so a later failure does not change it
  std::atomic<uint64_t> status_{0};
  EventRing events_;  // status_ values
  std::atomic<bool> restart_{false};
  std::atomic<bool> preset_change_{false};
  std::atomic<bool> fade_wanted_{false};
  std::atomic<bool> faded_out_{false};
  std::atomic<bool> running_{true};
  std::atomic<bool> stopped_{false};
  std::thread out_thread_;
  std::thread st_thread_;
};

// ---- stations (station.cpp) ----

// What a station thread runs: replaced as a whole by apply_config, never changed in place.
struct StationPlan {
  struct Input {
    std::shared_ptr<InputRx> rx;
    std::shared_ptr<DelayLine> delay;  // kept across plans while the input stays enabled
    double gain = 1.0;                 // linear
    double delay_s = 0;
  };
  // The enabled inputs in any order; an input's identity is its delay line (see Station).
  std::array<std::shared_ptr<const Input>, kStationInputs> inputs;
  // Where the mix goes: the processing worker when there is one (it feeds the streams), else
  // straight to the streams (the 1.0 path, bit for bit).
  std::shared_ptr<ProcessingWorker> processing;
  std::vector<std::shared_ptr<StreamWorker>> streams;
  uint32_t latency_frames = 0;  // the station reads every input this far behind "now"
  // A value other than the last plan's fades the station's output in over kRampFrames: the
  // engine's switch away from a processing worker that faded out (ProcessingWorker::fade_out).
  uint64_t fade_in = 0;
};

// The station thread: every 10 ms reads each input ring up to "now - latency", feeds the delay
// lines, mixes and hands the mix to every stream worker (or to the processing worker).
class Station {
 public:
  static constexpr int64_t kTickNs = 10'000'000;
  static constexpr uint32_t kPassFrames = DelayLine::kHeadroomFrames;  // 0.5 s per write/read pass
  static constexpr uint64_t kMaxCatchUpFrames = kStationRate;          // a longer backlog is skipped

  Station(const ClockSource& clock, std::shared_ptr<const StationPlan> plan);
  ~Station();  // stop()
  // Hands a new plan to the thread (taken at its next tick); objects the thread let go of
  // since the last call are released here, on the caller's thread.
  void publish(std::shared_ptr<const StationPlan> plan);
  // Releases, on the caller's thread, what the station thread let go of (e.g. an old input's
  // socket, still joined to its group) without waiting for the next publish().
  void collect();
  void stop();
  int64_t started_ns() const { return started_ns_; }
  // The peaks of the input with this delay line (its identity), if the station plays it now.
  std::optional<std::array<float, 2>> input_peak(const DelayLine* input) const;
  std::array<float, 2> mix_peak() const;
  uint64_t skipped_frames() const { return skipped_.load(std::memory_order_relaxed); }

 private:
  struct Active {  // station thread only
    std::shared_ptr<const StationPlan::Input> input;
    double gain = -1;
    double delay_s = -1;
    bool leaving = false;
  };
  void run();
  void take_plan();
  void place_joins();  // gives plan inputs not playing yet a free slot (fade in)
  void apply_settings(int slot, const StationPlan::Input& w);
  void retire(std::shared_ptr<const void> p);
  void process(uint64_t from, size_t frames);
  void publish_peaks();

  const ClockSource& clock_;
  const int64_t started_ns_;

  std::mutex plan_m_;
  std::shared_ptr<const StationPlan> pending_;
  std::vector<std::shared_ptr<const void>> graveyard_;  // released by publish()/stop(), not here
  std::atomic<bool> has_pending_{false};

  // station thread only
  std::shared_ptr<const StationPlan> plan_;
  std::array<Active, kStationInputs> active_;
  StationMixer mixer_;
  std::array<std::vector<int32_t>, 2> pcm_;                 // kPassFrames per side
  std::array<std::vector<float>, kStationInputs> in_;      // kPassFrames stereo per input
  std::vector<float> mix_;                                  // kPassFrames stereo
  GainRamp out_{1.0};       // the output level (StationPlan::fade_in); 1 except while fading in
  uint64_t fade_in_ = 0;    // the plan value it last faded in for
  uint64_t pos_ = 0;  // next timeline sample to mix
  bool positioned_ = false;
  uint64_t step_generation_ = 0;
  uint64_t frames_since_peaks_ = 0;

  std::array<std::atomic<float>, kStationInputs * 2> input_peak_{};
  std::array<std::atomic<const DelayLine*>, kStationInputs> owner_{};  // whose peaks a slot holds
  std::array<std::atomic<float>, 2> mix_peak_{};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace bam::encoder::detail
