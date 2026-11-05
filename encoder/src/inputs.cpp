// SPDX-License-Identifier: GPL-3.0-or-later
// AES67 inputs of the Bounce Encoder: one socket and RxStream per enabled input, writing a ring
// pair in process memory on the media clock (spec §4.1), and the network thread receiving them.
#include <algorithm>
#include <cmath>

#include "bam/config.hpp"  // ptime_us_to_frames
#include "bam/constants.hpp"
#include "bam/platform/time.hpp"
#include "bam/sdp.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

namespace {
constexpr int64_t kTickNs = 5'000'000;  // RxStream::on_tick (underrun counting)
}  // namespace

float level_db(float linear) {
  if (!(linear > 0.0f)) return -120.0f;
  return std::max(-120.0f, 20.0f * std::log10(linear));
}

std::string input_key(const InputConfig& in, const std::string& iface) {
  return in.sdp + '|' + std::to_string(in.latency_us) + '|' + iface;
}

std::shared_ptr<InputRx> make_input_rx(const InputConfig& in, const std::string& iface) {
  auto rx = std::make_shared<InputRx>();
  rx->key = input_key(in, iface);
  rx->latency_frames = ptime_us_to_frames(in.latency_us);
  const SdpParseResult parsed = parse_sdp(in.sdp);
  if (!parsed.stream) {
    rx->error = "nieprawidłowy SDP: " + parsed.error;
    return rx;
  }
  const SdpStream& s = *parsed.stream;
  rx->name = s.session_name;
  rx->group = s.multicast_address;
  rx->source = s.source_filter;
  rx->port = s.port;
  for (int ch = 0; ch < 2; ++ch) {
    rx->samples[ch].assign(kRingFrames, 0);
    rx->tags[ch].assign(kBlocksPerRing, 0);
    rx->rings[ch] = ChannelRing(rx->samples[ch].data(), rx->tags[ch].data());
  }
  std::string err;
  auto sock = platform::UdpSocket::open(s.port, &err);
  if (!sock) {
    rx->error = "gniazdo odbiorcze: " + err;
    return rx;
  }
  if (!sock->join(s.multicast_address, iface, s.source_filter, &err)) {
    rx->error = "dołączenie do grupy " + s.multicast_address + ": " + err;
    return rx;
  }
  rx->socket = std::move(*sock);
  rx->stream = std::make_unique<RxStream>(s, rx->rings[0], rx->rings[1], rx->latency_frames, 1.0);
  return rx;
}

RxThread::RxThread(const ClockSource& clock) : clock_(clock), inputs_(std::make_shared<RxSet>()) {
  thread_ = std::thread([this] { run(); });
}

RxThread::~RxThread() { stop(); }

void RxThread::set_inputs(std::shared_ptr<const RxSet> inputs) {
  std::lock_guard lock(m_);
  inputs_ = std::move(inputs);
}

std::shared_ptr<const RxSet> RxThread::snapshot() const {
  std::lock_guard lock(m_);
  return inputs_;
}

void RxThread::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void RxThread::run() {
  platform::set_realtime_priority("Pro Audio");
  std::vector<uint8_t> buf(2048);
  std::vector<uint8_t> ready;
  std::vector<platform::UdpSocket*> socks;
  std::vector<InputRx*> owners;
  std::string from_ip, to_ip;
  int64_t last_tick = 0;
  while (running_) {
    const auto set = snapshot();  // keeps every input of this pass alive
    socks.clear();
    owners.clear();
    for (const auto& in : *set) {
      if (!in->stream) continue;
      socks.push_back(&in->socket);
      owners.push_back(in.get());
    }
    platform::wait_readable(socks, 2, ready);

    const MediaClock clk = clock_.get();
    for (size_t i = 0; i < socks.size(); ++i) {
      if (!ready[i]) continue;
      InputRx& in = *owners[i];
      int n;
      while ((n = socks[i]->recv(buf, &from_ip, &to_ip)) >= 0) {
        // a socket may see every group joined on its port (Windows, Linux): only our group,
        // and only our sender when the SDP names one
        if (to_ip != in.group) continue;
        if (!in.source.empty() && from_ip != in.source) continue;
        const int64_t now = platform::monotonic_ns();
        in.stream->on_packet(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), clk.sample_at(now), now);
      }
    }
    const int64_t now = platform::monotonic_ns();
    if (now - last_tick >= kTickNs) {
      const uint64_t now_s = clk.sample_at(now);
      for (InputRx* in : owners) in->stream->on_tick(now_s);
      last_tick = now;
    }
  }
}

}  // namespace bam::encoder::detail
