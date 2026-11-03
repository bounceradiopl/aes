// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bam/clock.hpp"
#include "bam/constants.hpp"
#include "bam/timeline.hpp"

namespace bam {

inline constexpr uint32_t kShmMagic = 0x42414D33;  // "BAM3"
inline constexpr uint32_t kShmVersion = 3;
inline constexpr const char* kShmName = "BounceAESMaster.v3";
inline constexpr const char* kPeriodEventName = "BounceAESMaster.v3.period";
inline constexpr size_t kDeviceNameBytes = 48;
inline constexpr int kWinPairs = 16;              // Windows source pairs on the timeline
inline constexpr int kWinChannels = kWinPairs * 2;
inline constexpr size_t kBridgeBufferBytes = 65536;
inline constexpr int kMixBuses = 32;              // stereo mix-bus ring pairs
inline constexpr int kMixChannels = kMixBuses * 2;

enum class ShmPtpState : uint32_t { Listening = 0, Locked = 1, Holdover = 2, FreeRun = 3, Master = 4 };

// Where an ASIO input pair (or a Windows output) reads its audio from.
enum class ShmSource : uint32_t { None = 0, Net = 1, Asio = 2, Win = 3, Mix = 4 };

struct ShmAsioRoute {
  std::atomic<uint32_t> kind{0};          // ShmSource
  std::atomic<uint32_t> pair{0};          // source pair index
  std::atomic<uint32_t> delay_frames{0};  // D_src
};

// Shared between BounceAESService (creates it), the ASIO driver, and the audio bridge.
// Only lock-free atomics and plain data, so the layout is identical in every process.
struct ShmHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t ring_frames;
  uint32_t block_frames;
  uint32_t channels_net_in;
  uint32_t channels_asio_out;
  uint32_t channels_win_src;
  uint32_t header_size;

  // service -> ASIO client
  std::atomic<uint64_t> service_generation;
  std::atomic<uint64_t> heartbeat;
  std::atomic<uint64_t> period_counter;
  std::atomic<uint64_t> period_start_sample;
  std::atomic<uint32_t> ptp_state;
  std::atomic<uint32_t> output_latency_frames;
  std::atomic<uint32_t> device_active_mask;      // which ASIO pairs carry audio (any route)
  // media clock mapping (local monotonic ns <-> PTP ns), for processes that run their own audio
  // clock (the audio bridge); QueryPerformanceCounter is system wide on Windows
  std::atomic<uint32_t> clock_version;           // seqlock
  std::atomic<int64_t> clock_base_local;
  std::atomic<int64_t> clock_base_ptp;
  std::atomic<uint64_t> clock_rate_bits;         // double
  std::atomic<uint64_t> clock_step_generation;   // changes when the clock jumps
  ShmAsioRoute asio_routes[kDeviceCount];        // where each ASIO input pair reads from
  std::atomic<uint32_t> routes_version;          // seqlock over asio_routes
  std::atomic<uint32_t> names_version;           // seqlock over device_names
  char device_names[kDeviceCount][kDeviceNameBytes];

  // ASIO client -> service
  std::atomic<uint32_t> buffer_frames;
  std::atomic<uint32_t> client_pid;
  std::atomic<uint32_t> client_running;
  std::atomic<uint64_t> client_heartbeat;

  // service <-> audio bridge (user session process)
  std::atomic<uint32_t> bridge_pid;
  std::atomic<uint64_t> bridge_heartbeat;
  std::atomic<uint32_t> win_src_delay_frames;
  std::atomic<uint32_t> request_version;  // seqlock, service -> bridge JSON
  std::atomic<uint32_t> request_len;
  std::atomic<uint32_t> report_version;   // seqlock, bridge -> service JSON
  std::atomic<uint32_t> report_len;
  char request[kBridgeBufferBytes];
  char report[kBridgeBufferBytes];

  uint32_t channels_mix;
};

struct ShmLayout {
  static size_t header_bytes();
  static size_t net_in_samples_offset(int ch);
  static size_t asio_out_samples_offset(int ch);
  static size_t win_src_samples_offset(int ch);
  static size_t net_in_tags_offset(int ch);
  static size_t asio_out_tags_offset(int ch);
  static size_t win_src_tags_offset(int ch);
  static size_t mix_samples_offset(int ch);
  static size_t mix_tags_offset(int ch);
  static size_t total_bytes();
};

class ShmView {
 public:
  explicit ShmView(void* base) : base_(static_cast<std::byte*>(base)) {}

  static void initialize(void* base, uint64_t generation);
  bool compatible() const;
  ShmHeader& header() const { return *reinterpret_cast<ShmHeader*>(base_); }

  ChannelRing net_in(int ch) const;    // ch 0..63: received AES67 subscriptions
  ChannelRing asio_out(int ch) const;  // ch 0..63: what the ASIO host plays
  ChannelRing win_src(int ch) const;   // ch 0..31: captured Windows audio
  ChannelRing mix_bus(int ch) const;   // ch 0..63: mix bus outputs
  ChannelRing source_ring(ShmSource kind, int ch) const;  // dispatch by kind; None -> invalid ring

  void set_asio_route(int pair, ShmSource kind, int src_pair, uint32_t delay_frames);  // seqlock write
  struct AsioRoute {
    ShmSource kind = ShmSource::None;
    int pair = 0;
    uint32_t delay = 0;
  };
  AsioRoute asio_route(int pair) const;  // seqlock read

  void publish_clock(const MediaClock& clock, uint64_t step_generation);  // seqlock write
  MediaClock read_clock(uint64_t* step_generation) const;                 // seqlock read

  void set_device_name(int dev, std::string_view name);
  std::string device_name(int dev) const;

  void write_request(std::string_view json);  // service -> bridge
  std::string read_request(uint32_t* version) const;
  void write_report(std::string_view json);  // bridge -> service
  std::string read_report(uint32_t* version) const;

 private:
  std::byte* base_;
};

}  // namespace bam
