// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/asio_bridge.hpp"

#include <cstdio>
#include <cstring>

namespace bam {

void AsioBridge::read_inputs(uint64_t period_start, uint32_t frames, int32_t* const* in_bufs) const {
  // Each ASIO input pair reads the pair it is routed to, delayed by that source's readiness (D_src).
  for (int pair = 0; pair < kDeviceCount; ++pair) {
    const auto route = view_.asio_route(pair);
    for (int side = 0; side < 2; ++side) {
      int32_t* dst = in_bufs[pair * 2 + side];
      if (!dst) continue;
      const uint64_t lag = uint64_t(frames) + route.delay;
      if (route.kind == ShmSource::None || period_start < lag) {
        std::memset(dst, 0, frames * sizeof(int32_t));
        continue;
      }
      view_.source_ring(route.kind, route.pair * 2 + side).read(period_start - lag, dst, frames);
    }
  }
}

void AsioBridge::write_outputs(uint64_t period_start, uint32_t frames, const int32_t* const* out_bufs) {
  for (int ch = 0; ch < kChannelCount; ++ch) {
    view_.asio_out(ch).write(output_start(period_start, frames), out_bufs[ch], frames);
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
