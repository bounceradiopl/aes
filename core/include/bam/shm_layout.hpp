// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bam/constants.hpp"
#include "bam/timeline.hpp"

namespace bam {

inline constexpr uint32_t kShmMagic = 0x42414D31;  // "BAM1"
inline constexpr uint32_t kShmVersion = 1;
inline constexpr const char* kShmName = "BounceAESMaster.v1";
inline constexpr const char* kPeriodEventName = "BounceAESMaster.v1.period";
inline constexpr size_t kDeviceNameBytes = 48;

enum class ShmPtpState : uint32_t { Listening = 0, Locked = 1, Holdover = 2, FreeRun = 3 };

// Shared between BounceAESService (creates it) and the ASIO driver living in the audio application.
// Only lock-free atomics and plain data, so the layout is identical in both processes.
struct ShmHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t ring_frames;
  uint32_t block_frames;
  uint32_t channels_in;
  uint32_t channels_out;
  uint32_t header_size;

  // service -> client
  std::atomic<uint64_t> service_generation;  // changes on every service start
  std::atomic<uint64_t> heartbeat;           // incremented by the service at least every 10 ms
  std::atomic<uint64_t> period_counter;      // incremented on every ASIO period boundary
  std::atomic<uint64_t> period_start_sample;  // media-clock sample of the current period start
  std::atomic<uint32_t> ptp_state;            // ShmPtpState
  std::atomic<uint32_t> output_latency_frames;
  std::atomic<uint32_t> device_active_mask;
  std::atomic<uint32_t> rx_latency_frames[kDeviceCount];
  std::atomic<uint32_t> names_version;  // seqlock: odd while device_names is being written
  char device_names[kDeviceCount][kDeviceNameBytes];

  // client -> service
  std::atomic<uint32_t> buffer_frames;   // ASIO buffer size chosen by the host
  std::atomic<uint32_t> client_pid;      // 0 = free; claimed with compare_exchange
  std::atomic<uint32_t> client_running;  // 1 between ASIO start() and stop()
  std::atomic<uint64_t> client_heartbeat;
};

struct ShmLayout {
  static size_t header_bytes();
  static size_t in_samples_offset(int ch);
  static size_t out_samples_offset(int ch);
  static size_t in_tags_offset(int ch);
  static size_t out_tags_offset(int ch);
  static size_t total_bytes();
};

class ShmView {
 public:
  explicit ShmView(void* base) : base_(static_cast<std::byte*>(base)) {}

  // Constructs the header in place and clears all rings. Called by the service only.
  static void initialize(void* base, uint64_t generation);

  bool compatible() const;
  ShmHeader& header() const { return *reinterpret_cast<ShmHeader*>(base_); }
  ChannelRing input(int ch) const;
  ChannelRing output(int ch) const;

  void set_device_name(int dev, std::string_view name);  // dev 0..31, truncated to 47 bytes
  std::string device_name(int dev) const;

 private:
  std::byte* base_;
};

}  // namespace bam
