// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/shm_layout.hpp"

#include <cstring>
#include <new>

namespace bam {

namespace {
constexpr size_t kPage = 4096;
constexpr size_t kRingBytes = size_t(kRingFrames) * sizeof(int32_t);
constexpr size_t kTagBytes = size_t(kBlocksPerRing) * sizeof(uint32_t);

size_t utf8_safe_length(std::string_view s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s.size();
  size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // don't split a sequence
  return n;
}
}  // namespace

size_t ShmLayout::header_bytes() { return (sizeof(ShmHeader) + kPage - 1) / kPage * kPage; }
size_t ShmLayout::in_samples_offset(int ch) { return header_bytes() + size_t(ch) * kRingBytes; }
size_t ShmLayout::out_samples_offset(int ch) { return in_samples_offset(kChannelCount) + size_t(ch) * kRingBytes; }
size_t ShmLayout::in_tags_offset(int ch) { return out_samples_offset(kChannelCount) + size_t(ch) * kTagBytes; }
size_t ShmLayout::out_tags_offset(int ch) { return in_tags_offset(kChannelCount) + size_t(ch) * kTagBytes; }
size_t ShmLayout::total_bytes() { return out_tags_offset(kChannelCount); }

void ShmView::initialize(void* base, uint64_t generation) {
  std::memset(base, 0, ShmLayout::total_bytes());
  auto* h = new (base) ShmHeader();
  h->magic = kShmMagic;
  h->version = kShmVersion;
  h->sample_rate = kSampleRate;
  h->ring_frames = kRingFrames;
  h->block_frames = kBlockFrames;
  h->channels_in = kChannelCount;
  h->channels_out = kChannelCount;
  h->header_size = static_cast<uint32_t>(sizeof(ShmHeader));
  h->service_generation.store(generation);
}

bool ShmView::compatible() const {
  const auto& h = header();
  return h.magic == kShmMagic && h.version == kShmVersion && h.sample_rate == kSampleRate &&
         h.ring_frames == kRingFrames && h.block_frames == kBlockFrames && h.channels_in == kChannelCount &&
         h.channels_out == kChannelCount && h.header_size == sizeof(ShmHeader);
}

ChannelRing ShmView::input(int ch) const {
  return ChannelRing(reinterpret_cast<int32_t*>(base_ + ShmLayout::in_samples_offset(ch)),
                     reinterpret_cast<uint32_t*>(base_ + ShmLayout::in_tags_offset(ch)));
}

ChannelRing ShmView::output(int ch) const {
  return ChannelRing(reinterpret_cast<int32_t*>(base_ + ShmLayout::out_samples_offset(ch)),
                     reinterpret_cast<uint32_t*>(base_ + ShmLayout::out_tags_offset(ch)));
}

void ShmView::set_device_name(int dev, std::string_view name) {
  auto& h = header();
  h.names_version.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
  const size_t n = utf8_safe_length(name, kDeviceNameBytes - 1);
  std::memset(h.device_names[dev], 0, kDeviceNameBytes);
  std::memcpy(h.device_names[dev], name.data(), n);
  h.names_version.fetch_add(1, std::memory_order_acq_rel);  // even: stable
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

}  // namespace bam
