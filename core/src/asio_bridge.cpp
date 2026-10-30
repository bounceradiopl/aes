// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/asio_bridge.hpp"

#include <cstdio>
#include <cstring>

namespace bam {

void AsioBridge::read_inputs(uint64_t period_start, uint32_t frames, int32_t* const* in_bufs) const {
  auto& h = view_.header();
  for (int ch = 0; ch < kChannelCount; ++ch) {
    int32_t* dst = in_bufs[ch];
    if (!dst) continue;
    const uint64_t lag = uint64_t(frames) + h.rx_latency_frames[ch / 2].load(std::memory_order_relaxed);
    if (period_start < lag) {
      std::memset(dst, 0, frames * sizeof(int32_t));
      continue;
    }
    view_.input(ch).read(period_start - lag, dst, frames);
  }
}

void AsioBridge::write_outputs(uint64_t period_start, uint32_t frames, const int32_t* const* out_bufs) {
  for (int ch = 0; ch < kChannelCount; ++ch) {
    view_.output(ch).write(period_start + frames, out_bufs[ch], frames);
  }
}

std::string channel_label(int channel, std::string_view device_name) {
  constexpr size_t kMax = 31;
  char base[24];
  const int dev = channel / 2 + 1;
  std::snprintf(base, sizeof(base), "BounceAES %02d", dev);
  std::string label = std::string(base) + (channel % 2 ? " R" : " L");
  if (!device_name.empty() && device_name != base) label += " (" + std::string(device_name) + ")";
  if (label.size() > kMax) {
    size_t n = kMax;
    while (n > 0 && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80) --n;  // keep UTF-8 sequences whole
    label.resize(n);
  }
  return label;
}

}  // namespace bam
