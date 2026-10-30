// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/shm_layout.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace bam {

namespace {
constexpr size_t kPage = 4096;
constexpr size_t kRingBytes = size_t(kRingFrames) * sizeof(int32_t);
constexpr size_t kTagBytes = size_t(kBlocksPerRing) * sizeof(uint32_t);
// Ring groups laid out back to back after the header: net_in, asio_out, win_src.
constexpr int kNetInCh = kChannelCount;      // 64
constexpr int kAsioOutCh = kChannelCount;    // 64
constexpr int kWinSrcCh = kWinChannels;      // 32
constexpr size_t kSamplesBytes = size_t(kNetInCh + kAsioOutCh + kWinSrcCh) * kRingBytes;

size_t utf8_safe_length(std::string_view s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s.size();
  size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return n;
}
}  // namespace

size_t ShmLayout::header_bytes() { return (sizeof(ShmHeader) + kPage - 1) / kPage * kPage; }

size_t ShmLayout::net_in_samples_offset(int ch) { return header_bytes() + size_t(ch) * kRingBytes; }
size_t ShmLayout::asio_out_samples_offset(int ch) { return net_in_samples_offset(kNetInCh) + size_t(ch) * kRingBytes; }
size_t ShmLayout::win_src_samples_offset(int ch) { return asio_out_samples_offset(kAsioOutCh) + size_t(ch) * kRingBytes; }
size_t ShmLayout::net_in_tags_offset(int ch) { return header_bytes() + kSamplesBytes + size_t(ch) * kTagBytes; }
size_t ShmLayout::asio_out_tags_offset(int ch) { return net_in_tags_offset(kNetInCh) + size_t(ch) * kTagBytes; }
size_t ShmLayout::win_src_tags_offset(int ch) { return asio_out_tags_offset(kAsioOutCh) + size_t(ch) * kTagBytes; }
size_t ShmLayout::total_bytes() { return win_src_tags_offset(kWinSrcCh); }

void ShmView::initialize(void* base, uint64_t generation) {
  std::memset(base, 0, ShmLayout::total_bytes());
  auto* h = new (base) ShmHeader();
  h->magic = kShmMagic;
  h->version = kShmVersion;
  h->sample_rate = kSampleRate;
  h->ring_frames = kRingFrames;
  h->block_frames = kBlockFrames;
  h->channels_net_in = kNetInCh;
  h->channels_asio_out = kAsioOutCh;
  h->channels_win_src = kWinSrcCh;
  h->header_size = static_cast<uint32_t>(sizeof(ShmHeader));
  h->service_generation.store(generation);
}

bool ShmView::compatible() const {
  const auto& h = header();
  return h.magic == kShmMagic && h.version == kShmVersion && h.sample_rate == kSampleRate &&
         h.ring_frames == kRingFrames && h.block_frames == kBlockFrames && h.channels_net_in == kNetInCh &&
         h.channels_asio_out == kAsioOutCh && h.channels_win_src == kWinSrcCh && h.header_size == sizeof(ShmHeader);
}

ChannelRing ShmView::net_in(int ch) const {
  return ChannelRing(reinterpret_cast<int32_t*>(base_ + ShmLayout::net_in_samples_offset(ch)),
                     reinterpret_cast<uint32_t*>(base_ + ShmLayout::net_in_tags_offset(ch)));
}
ChannelRing ShmView::asio_out(int ch) const {
  return ChannelRing(reinterpret_cast<int32_t*>(base_ + ShmLayout::asio_out_samples_offset(ch)),
                     reinterpret_cast<uint32_t*>(base_ + ShmLayout::asio_out_tags_offset(ch)));
}
ChannelRing ShmView::win_src(int ch) const {
  return ChannelRing(reinterpret_cast<int32_t*>(base_ + ShmLayout::win_src_samples_offset(ch)),
                     reinterpret_cast<uint32_t*>(base_ + ShmLayout::win_src_tags_offset(ch)));
}

ChannelRing ShmView::source_ring(ShmSource kind, int ch) const {
  switch (kind) {
    case ShmSource::Net:
      return net_in(ch);
    case ShmSource::Asio:
      return asio_out(ch);
    case ShmSource::Win:
      return ch < kWinChannels ? win_src(ch) : ChannelRing{};
    case ShmSource::None:
      break;
  }
  return ChannelRing{};
}

void ShmView::set_asio_route(int pair, ShmSource kind, int src_pair, uint32_t delay_frames) {
  auto& h = header();
  h.routes_version.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
  h.asio_routes[pair].kind.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
  h.asio_routes[pair].pair.store(static_cast<uint32_t>(src_pair), std::memory_order_relaxed);
  h.asio_routes[pair].delay_frames.store(delay_frames, std::memory_order_relaxed);
  h.routes_version.fetch_add(1, std::memory_order_acq_rel);  // even: stable
}

ShmView::AsioRoute ShmView::asio_route(int pair) const {
  const auto& h = header();
  AsioRoute r;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const uint32_t v1 = h.routes_version.load(std::memory_order_acquire);
    if (v1 & 1) continue;
    r.kind = static_cast<ShmSource>(h.asio_routes[pair].kind.load(std::memory_order_relaxed));
    r.pair = static_cast<int>(h.asio_routes[pair].pair.load(std::memory_order_relaxed));
    r.delay = h.asio_routes[pair].delay_frames.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (h.routes_version.load(std::memory_order_acquire) == v1) break;
  }
  return r;
}

void ShmView::set_device_name(int dev, std::string_view name) {
  auto& h = header();
  h.names_version.fetch_add(1, std::memory_order_acq_rel);
  const size_t n = utf8_safe_length(name, kDeviceNameBytes - 1);
  std::memset(h.device_names[dev], 0, kDeviceNameBytes);
  std::memcpy(h.device_names[dev], name.data(), n);
  h.names_version.fetch_add(1, std::memory_order_acq_rel);
}

std::string ShmView::device_name(int dev) const {
  const auto& h = header();
  char buf[kDeviceNameBytes];
  for (int attempt = 0; attempt < 100; ++attempt) {
    const uint32_t v1 = h.names_version.load(std::memory_order_acquire);
    if (v1 & 1) continue;
    std::memcpy(buf, h.device_names[dev], kDeviceNameBytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (h.names_version.load(std::memory_order_acquire) == v1) break;
  }
  buf[kDeviceNameBytes - 1] = 0;
  return std::string(buf);
}

namespace {
void seqlock_write(std::atomic<uint32_t>& version, std::atomic<uint32_t>& len, char* buf, std::string_view json) {
  const size_t n = std::min(json.size(), kBridgeBufferBytes - 1);
  version.fetch_add(1, std::memory_order_acq_rel);
  std::memcpy(buf, json.data(), n);
  buf[n] = 0;
  len.store(static_cast<uint32_t>(n), std::memory_order_relaxed);
  version.fetch_add(1, std::memory_order_acq_rel);
}

std::string seqlock_read(const std::atomic<uint32_t>& version, const std::atomic<uint32_t>& len, const char* buf,
                         uint32_t* out_version) {
  std::string result;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const uint32_t v1 = version.load(std::memory_order_acquire);
    if (v1 & 1) continue;
    const size_t n = std::min<size_t>(len.load(std::memory_order_relaxed), kBridgeBufferBytes - 1);
    result.assign(buf, n);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (version.load(std::memory_order_acquire) == v1) {
      if (out_version) *out_version = v1;
      return result;
    }
  }
  return result;
}
}  // namespace

void ShmView::write_request(std::string_view json) { seqlock_write(header().request_version, header().request_len, header().request, json); }
std::string ShmView::read_request(uint32_t* version) const {
  return seqlock_read(header().request_version, header().request_len, header().request, version);
}
void ShmView::write_report(std::string_view json) { seqlock_write(header().report_version, header().report_len, header().report, json); }
std::string ShmView::read_report(uint32_t* version) const {
  return seqlock_read(header().report_version, header().report_len, header().report, version);
}

}  // namespace bam
