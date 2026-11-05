// SPDX-License-Identifier: GPL-3.0-or-later
// Internal pieces of the Bounce Encoder engine (engine.hpp): inputs, the PTP slave, stations and
// stream workers. Everything here is built on an API thread and handed to the audio threads
// whole; see engine.hpp for the thread layout.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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
// make_input_rx() except for the RxStream state, which only the network thread touches.
struct InputRx {
  std::string key;  // SDP | latency | interface: another key needs a new InputRx
  std::string name;
  std::string group;
  std::string source;  // sender address from a=source-filter, may be empty
  uint16_t port = 0;
  uint32_t latency_frames = 0;
  std::string error;  // Polish; set when the SDP or the socket failed (then stream is null)
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

// The network thread: receives every input of the current set (re-read every pass), dispatching
// a datagram only to the input whose group (and source, when filtered) it was sent to.
class RxThread {
 public:
  explicit RxThread(const ClockSource& clock);
  ~RxThread();  // stop()
  void set_inputs(std::shared_ptr<const RxSet> inputs);
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
  bool ever_stepped() const { return ever_stepped_.load(); }
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
  std::atomic<bool> ever_stepped_{false};
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

// ---- stations (station.cpp) ----

// What a station thread runs: replaced as a whole by apply_config, never changed in place.
struct StationPlan {
  struct Input {
    std::shared_ptr<InputRx> rx;
    std::shared_ptr<DelayLine> delay;  // kept across plans while the input stays enabled
    double gain = 1.0;                 // linear
    double delay_s = 0;
  };
  std::array<std::shared_ptr<const Input>, kStationInputs> inputs;  // null: no enabled input there
  std::vector<std::shared_ptr<StreamWorker>> streams;
  uint32_t latency_frames = 0;  // the station reads every input this far behind "now"
};

// The station thread: every 10 ms reads each input ring up to "now - latency", feeds the delay
// lines, mixes and hands the mix to every stream worker.
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
  std::array<std::array<float, 2>, kStationInputs> input_peaks() const;
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
  uint64_t pos_ = 0;  // next timeline sample to mix
  bool positioned_ = false;
  uint64_t step_generation_ = 0;
  uint64_t frames_since_peaks_ = 0;

  std::array<std::atomic<float>, kStationInputs * 2> input_peak_{};
  std::array<std::atomic<float>, 2> mix_peak_{};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace bam::encoder::detail
