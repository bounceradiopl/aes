// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine end to end (spec §9 "Cały tor"): test AES67 senders on loopback multicast
// (127.0.0.1 only) -> inputs from SDP -> 2 s delay and a -6 dB second input -> station mix ->
// FLAC 48 kHz -> a fake Icecast server on 127.0.0.1 -> decoded bit for bit against the expected
// mix. Also: editing one stream restarts only that stream (the other keeps its connection),
// disabling the station closes its connections, and the PTP slave follows a BounceAES Master
// engine acting as PTP master on loopback (domain 87). Elsewhere PTP stays off (free-run), but
// for the AES67 output tests of its PTP gating.
// Stereo Tool processing (1.1) runs on the fake library of tests/fake_stereotool: the station's
// path through it bit for bit, switching it on and off without silence, presets, save, restart,
// the automatic retries and the status. The AES67 output of a station (1.2) is received on
// 127.0.0.1: bit for bit at its delay D, through Stereo Tool, never repeating audio when
// processing is switched, waiting for the first PTP lock, holdover, gaps and disabling it.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bam/constants.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "bam/rtp.hpp"
#include "bam/sap.hpp"
#include "bam/sdp.hpp"
#include "bam/timeline.hpp"
#include "test_util.hpp"
#include "../encoder/src/aes67_sap.hpp"
#include "../encoder/src/aes67_sender.hpp"     // the failing-interface seam
#include "../encoder/src/engine_internal.hpp"  // AudioQueue (the station -> stream handover)
#include "../encoder/src/stereotool_api.hpp"   // the web interface parameter index
#include "bam/log.hpp"
#include "fake_stereotool/fake_control.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;

namespace {

constexpr const char* kLoopback = "127.0.0.1";  // tests never touch a real network

// Polls `pred` every 10 ms until it holds or `timeout_ms` passes.
bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
  const int64_t deadline = bp::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (bp::monotonic_ns() < deadline) {
    if (pred()) return true;
    bp::sleep_ns(10'000'000);
  }
  return pred();
}

// ---- test signals, functions of the absolute media-clock sample ----

uint32_t mix_hash(uint64_t t, int ch, uint64_t salt) {
  uint64_t x = t * 2 + uint64_t(ch) + salt * 0x9E3779B97F4A7C15ull;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return uint32_t(x);
}

// Input A: 24-bit noise within ±0.5 full scale (exact in float).
int32_t sig_a(uint64_t t, int ch) {
  const int32_t m = int32_t(mix_hash(t, ch, 1) >> 9) - (1 << 22);
  return m * 256;
}

// Input B: signed powers of two (or 0) up to 0.25 full scale, so B times any float gain is exact
// in float and the expected mix does not depend on how the compiler fuses multiply-adds.
int32_t sig_b(uint64_t t, int ch) {
  const uint32_t h = mix_hash(t, ch, 2);
  if ((h & 7) == 0) return 0;
  const int k = 2 + int((h >> 3) % 10);
  const int32_t v = int32_t(1) << (31 - k);
  return (h >> 16) & 1 ? -v : v;
}

constexpr uint64_t kDelayFrames = 96000;  // input A: 2 s
constexpr double kGainBDb = -6.0;

float to_float(int32_t s) { return float(s) * (1.0f / 2147483648.0f); }

int32_t pcm16(float x) {  // the encoder's float -> 16-bit conversion (rounded, saturating)
  const float v = x * 32768.0f;
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return int32_t(std::lrint(v));
}

// The station mix at media-clock sample t, as 16-bit PCM: A delayed by 2 s plus B at -6 dB.
int32_t expected16(uint64_t t, int ch) {
  static const float g = float(std::pow(10.0, kGainBDb / 20.0));
  const float a = to_float(sig_a(t - kDelayFrames, ch));
  const float b = to_float(sig_b(t, ch)) * g;  // exact
  return pcm16(a + b);
}

// ---- a test AES67 sender (L24 stereo, 1 ms packets) on loopback multicast ----

std::string make_sdp(const std::string& name, const std::string& group, uint16_t port) {
  bam::SdpStream s;
  s.session_name = name;
  s.origin_address = kLoopback;  // also the source filter
  s.multicast_address = group;
  s.ttl = 1;
  s.port = port;
  s.payload_type = 98;
  s.encoding = bam::Encoding::L24;
  s.sample_rate = 48000;
  s.channels = 2;
  s.ptime_ms = 1.0;
  s.frames_per_packet = 48;
  return bam::generate_sdp(s, 1, 1);
}

struct AesSender {
  // Packets go out up to this far ahead of the media clock (receivers keep up to 0.5 s of future
  // packets), so a sender thread delayed by the test host never leaves a hole in the input.
  static constexpr uint64_t kLeadFrames = 9600;  // 200 ms
  static constexpr uint32_t kFrames = 48;

  const be::EncoderEngine& engine;
  std::string group;
  uint16_t port;
  int32_t (*value)(uint64_t, int);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> first_ts{0};
  std::atomic<uint64_t> sent{0};
  std::thread thread;

  AesSender(const be::EncoderEngine& e, std::string g, uint16_t p, int32_t (*v)(uint64_t, int))
      : engine(e), group(std::move(g)), port(p), value(v) {
    std::string err;
    auto sock = bp::UdpSocket::open(0, &err);
    REQUIRE_MESSAGE(sock, err);
    REQUIRE(sock->set_multicast_out(kLoopback, 1, true));
    thread = std::thread([this, s = std::move(*sock)]() mutable { run(s); });
  }
  ~AesSender() {
    stop = true;
    if (thread.joinable()) thread.join();
  }

  void run(bp::UdpSocket& sock) {
    std::vector<uint8_t> packet(bam::kRtpHeaderSize + kFrames * 6);
    std::vector<int32_t> l(kFrames), r(kFrames);
    uint64_t next = 0;
    uint16_t seq = 1;
    while (!stop) {
      const uint64_t now = engine.media_clock().sample_at(bp::monotonic_ns());
      if (next == 0) {
        next = now / kFrames * kFrames;
        first_ts = next;
      }
      while (next <= now + kLeadFrames) {
        for (uint32_t i = 0; i < kFrames; ++i) {
          l[i] = value(next + i, 0);
          r[i] = value(next + i, 1);
        }
        bam::RtpHeader h;
        h.payload_type = 98;
        h.sequence = seq++;
        h.timestamp = uint32_t(next);
        h.ssrc = 0x1234;
        bam::write_rtp_header(h, packet.data());
        const int32_t* planes[] = {l.data(), r.data()};
        bam::encode_l24(planes, 2, kFrames, packet.data() + bam::kRtpHeaderSize);
        sock.send_to(packet, group, port);
        next += kFrames;
        ++sent;
      }
      bp::sleep_ns(1'000'000);
    }
  }
};

// ---- a fake Icecast server on 127.0.0.1: accepts any number of source connections ----

// With refuse_busy, like a real Icecast it answers 403 to a source for a mountpoint another
// source still holds. "Still holds" is decided when the new connection is accepted, by looking
// at the older connections' sockets themselves (a close sent before the new connect is always
// seen), so the check does not depend on thread timing.
struct FakeIcecast {
  struct Conn {
    int id = 0;
    bp::TcpStream s;
    std::mutex io;  // around every read of `s` (the serving thread and the busy check)
    std::atomic<bool> peek_wanted{false};  // the busy check waits for `io`: the serving thread steps aside
    std::string mount;
    std::mutex m;
    std::vector<uint8_t> body;
    std::atomic<size_t> bytes{0};
    std::atomic<bool> open{true};
    bool refused = false;
    std::atomic<bool> holds_mount{false};  // answered 200: a source on its mountpoint
    int64_t accepted_ns = 0;
  };

  bp::TcpListener listener;
  std::atomic<bool> refuse_busy{false};
  std::atomic<int> refused{0};
  std::atomic<bool> stop{false};
  std::mutex m;
  std::vector<std::shared_ptr<Conn>> conns;
  std::vector<std::thread> serving;  // accept thread only
  std::thread acceptor;

  FakeIcecast() {
    std::string err;
    auto l = bp::TcpListener::listen(kLoopback, 0, &err);
    REQUIRE_MESSAGE(l, err);
    listener = std::move(*l);
    acceptor = std::thread([this] { accept_loop(); });
  }
  ~FakeIcecast() {
    stop = true;
    if (acceptor.joinable()) acceptor.join();
    for (auto& t : serving)
      if (t.joinable()) t.join();
  }
  uint16_t port() const { return listener.port(); }

  // True when the peer closed c (checked on its socket now, not when its thread gets to it).
  bool closed_now(Conn& c) {
    if (!c.open) return true;
    c.peek_wanted = true;
    std::lock_guard lock(c.io);
    c.peek_wanted = false;
    if (!c.open) return true;
    std::vector<uint8_t> buf(65536);
    for (int i = 0; i < 256; ++i) {  // bounded: a source that sends without pause is open
      const int n = c.s.recv_some(buf, 0);
      if (n < 0) {
        c.open = false;
        return true;
      }
      if (n == 0) return false;
      std::lock_guard l2(c.m);
      c.body.insert(c.body.end(), buf.begin(), buf.begin() + n);
      c.bytes = c.body.size();
    }
    return false;
  }

  void accept_loop() {
    int next_id = 1;
    while (!stop) {
      auto s = listener.accept(50);
      if (!s) continue;
      auto c = std::make_shared<Conn>();
      c->id = next_id++;
      c->accepted_ns = bp::monotonic_ns();
      c->s = std::move(*s);
      std::vector<std::shared_ptr<Conn>> busy;  // sources holding a mount at this moment
      {
        std::lock_guard lock(m);
        // only sources already on their mountpoint (a connection still sending its request
        // holds none, and its request must not be read here)
        for (const auto& o : conns)
          if (o->holds_mount && !closed_now(*o)) busy.push_back(o);
        conns.push_back(c);
      }
      serving.emplace_back([this, c, busy] { serve(*c, busy); });
    }
  }

  void serve(Conn& c, const std::vector<std::shared_ptr<Conn>>& busy) {
    std::string head;
    std::vector<uint8_t> chunk(65536);
    size_t end = std::string::npos;
    while (!stop && end == std::string::npos) {
      int n;
      while (c.peek_wanted) std::this_thread::yield();
      {
        std::lock_guard lock(c.io);
        n = c.s.recv_some(chunk, 2);
      }
      if (n < 0) {
        c.open = false;
        return;
      }
      head.append(reinterpret_cast<const char*>(chunk.data()), size_t(n));
      end = head.find("\r\n\r\n");
    }
    if (end == std::string::npos) return;
    // "PUT /mount HTTP/1.1"
    const size_t a = head.find(' '), b = head.find(' ', a + 1);
    const std::string mount = head.substr(a + 1, b - a - 1);
    bool taken = false;
    for (const auto& o : busy) {
      std::lock_guard lock(o->m);
      taken = taken || o->mount == mount;
    }
    if (refuse_busy && taken) {
      {
        std::lock_guard lock(c.m);
        c.mount = mount;
        c.refused = true;
      }
      ++refused;
      const std::string no = "HTTP/1.1 403 Forbidden\r\n\r\n";
      c.s.send_all(std::span(reinterpret_cast<const uint8_t*>(no.data()), no.size()), 2000);
      c.s.close();
      c.open = false;
      return;
    }
    {
      std::lock_guard lock(c.m);
      c.mount = mount;
      c.body.assign(head.begin() + std::ptrdiff_t(end + 4), head.end());
      c.bytes = c.body.size();
    }
    const std::string ok = "HTTP/1.1 200 OK\r\n\r\n";
    if (!c.s.send_all(std::span(reinterpret_cast<const uint8_t*>(ok.data()), ok.size()), 2000)) {
      c.open = false;
      return;
    }
    c.holds_mount = true;
    while (!stop && c.open) {
      int n;
      while (c.peek_wanted) std::this_thread::yield();
      {
        std::lock_guard lock(c.io);
        n = c.open ? c.s.recv_some(chunk, 2) : -1;
      }
      if (n < 0) break;
      if (n == 0) continue;
      std::lock_guard lock(c.m);
      c.body.insert(c.body.end(), chunk.begin(), chunk.begin() + n);
      c.bytes = c.body.size();
    }
    c.open = false;
  }

  // The newest accepted (not refused) connection to `mount`, or nullptr.
  std::shared_ptr<Conn> latest(const std::string& mount) {
    std::lock_guard lock(m);
    for (auto it = conns.rbegin(); it != conns.rend(); ++it) {
      std::lock_guard l2((*it)->m);
      if ((*it)->mount == mount && !(*it)->refused) return *it;
    }
    return nullptr;
  }
  std::vector<uint8_t> body(Conn& c) {
    std::lock_guard lock(c.m);
    return c.body;
  }
};

// ---- Ogg FLAC decoding of what the server received ----

struct Decoded {
  std::vector<int32_t> samples;  // interleaved
  unsigned sample_rate = 0, channels = 0, bits = 0;
  bool error = false;
};

// The complete Ogg pages of the last logical stream in `data` (from its BOS page on).
std::vector<uint8_t> last_ogg_stream(const std::vector<uint8_t>& data) {
  size_t pos = 0, bos = std::string::npos, complete = 0;
  while (pos + 27 <= data.size() && std::memcmp(data.data() + pos, "OggS", 4) == 0) {
    const size_t segs = data[pos + 26];
    if (pos + 27 + segs > data.size()) break;
    size_t len = 27 + segs;
    for (size_t i = 0; i < segs; ++i) len += data[pos + 27 + i];
    if (pos + len > data.size()) break;
    if (data[pos + 5] & 0x02) bos = pos;
    pos += len;
    complete = pos;
  }
  if (bos == std::string::npos) return {};
  return std::vector<uint8_t>(data.begin() + std::ptrdiff_t(bos), data.begin() + std::ptrdiff_t(complete));
}

Decoded decode_ogg_flac(const std::vector<uint8_t>& data) {
  struct State {
    const std::vector<uint8_t>* data;
    size_t pos = 0;
    Decoded* out;
  };
  Decoded d;
  State st{&data, 0, &d};
  FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
  auto read_cb = [](const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* cd) -> FLAC__StreamDecoderReadStatus {
    auto* s = static_cast<State*>(cd);
    const size_t n = std::min(*bytes, s->data->size() - s->pos);
    std::memcpy(buffer, s->data->data() + s->pos, n);
    s->pos += n;
    *bytes = n;
    return n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  };
  auto write_cb = [](const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[],
                     void* cd) -> FLAC__StreamDecoderWriteStatus {
    auto* s = static_cast<State*>(cd);
    for (unsigned i = 0; i < frame->header.blocksize; ++i)
      for (unsigned c = 0; c < frame->header.channels; ++c) s->out->samples.push_back(buffer[c][i]);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  };
  auto meta_cb = [](const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* cd) {
    auto* s = static_cast<State*>(cd);
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
      s->out->sample_rate = m->data.stream_info.sample_rate;
      s->out->channels = m->data.stream_info.channels;
      s->out->bits = m->data.stream_info.bits_per_sample;
    }
  };
  auto error_cb = [](const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* cd) {
    static_cast<State*>(cd)->out->error = true;
  };
  REQUIRE(FLAC__stream_decoder_init_ogg_stream(dec, read_cb, nullptr, nullptr, nullptr, nullptr, write_cb, meta_cb, error_cb,
                                               &st) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
  FLAC__stream_decoder_process_until_end_of_stream(dec);
  FLAC__stream_decoder_finish(dec);
  FLAC__stream_decoder_delete(dec);
  return d;
}

// ---- configuration ----

be::StreamConfig stream(const std::string& id, be::Codec codec, uint32_t kbps, uint32_t rate, uint16_t port,
                        const std::string& mount) {
  be::StreamConfig s;
  s.id = id;
  s.codec = codec;
  s.bitrate_kbps = kbps;
  s.sample_rate = rate;
  s.host = kLoopback;
  s.port = port;
  s.mount = mount;
  s.user = "source";
  s.password = "hackme";
  return s;
}

be::EncoderConfig ready(be::EncoderConfig c) {
  be::normalize_config(c);
  const auto issues = be::validate_config(c);
  for (const auto& i : issues) MESSAGE(i.field << ": " << i.message);
  REQUIRE(issues.empty());
  return c;
}

be::EncoderEngineOptions no_ptp() {
  be::EncoderEngineOptions o;
  o.enable_ptp = false;
  return o;
}

using SampleFn = std::function<int32_t(uint64_t, int)>;

uint64_t media_now(const be::EncoderEngine& e) { return e.media_clock().sample_at(bp::monotonic_ns()); }

// Receives an AES67 stream on 127.0.0.1 into its own rings (the encoder's output under test).
// While recording, it also copies every position from the start on (50 ms behind the clock, so
// the packets are in) into `rec`, for checks longer than the rings hold.
struct AesReceiver {
  std::array<std::vector<int32_t>, 2> samples;
  std::array<std::vector<uint32_t>, 2> tags;
  std::array<bam::ChannelRing, 2> rings;
  std::optional<bp::UdpSocket> sock;
  std::unique_ptr<bam::RxStream> rx;
  std::thread thread;
  std::atomic<bool> run{true};
  const be::EncoderEngine& engine;
  std::mutex rec_m;
  bool recording = false;
  uint64_t rec_from = 0, rec_next = 0;
  std::vector<std::array<int32_t, 2>> rec;

  AesReceiver(const be::EncoderEngine& e, const std::string& group, uint32_t latency_frames = 480) : engine(e) {
    for (int ch = 0; ch < 2; ++ch) {
      samples[ch].assign(bam::kRingFrames, 0);
      tags[ch].assign(bam::kBlocksPerRing, 0);
      rings[ch] = bam::ChannelRing(samples[ch].data(), tags[ch].data());
    }
    std::string err;
    sock = bp::UdpSocket::open(bam::kRtpPort, &err);
    REQUIRE_MESSAGE(sock, err);
    REQUIRE_MESSAGE(sock->join(group, kLoopback, "", &err), err);
    const auto sdp = bam::parse_sdp(make_sdp("rx", group, bam::kRtpPort));
    REQUIRE(sdp.stream);
    rx = std::make_unique<bam::RxStream>(*sdp.stream, rings[0], rings[1], latency_frames, 1.0);
    thread = std::thread([this, group] {
      std::vector<uint8_t> buf(2048);
      std::array<std::vector<int32_t>, 2> chunk;
      std::string to;
      while (run) {
        int n;
        // Windows and Linux hand every group joined on the port to each socket: only ours counts
        while ((n = sock->recv(buf, nullptr, &to)) >= 0)
          if (to == group)
            rx->on_packet(std::span<const uint8_t>(buf.data(), size_t(n)), media_now(engine), bp::monotonic_ns());
        {
          std::lock_guard lock(rec_m);
          const uint64_t upto = media_now(engine) - 2400;
          if (recording && upto > rec_next) {
            const size_t len = size_t(upto - rec_next);
            for (int ch = 0; ch < 2; ++ch) {
              chunk[ch].resize(len);
              rings[ch].read(rec_next, chunk[ch].data(), len);
            }
            for (size_t i = 0; i < len; ++i) rec.push_back({chunk[0][i], chunk[1][i]});
            rec_next = upto;
          }
        }
        bp::sleep_ns(200'000);
      }
    });
  }
  ~AesReceiver() {
    run = false;
    thread.join();
  }
  uint64_t packets() const { return rx->stats().packets; }
  int32_t at(uint64_t pos, int ch) const {
    int32_t v;
    rings[ch].read(pos, &v, 1);
    return v;
  }
  void start_recording(uint64_t from) {
    std::lock_guard lock(rec_m);
    rec.clear();
    rec_from = rec_next = from;
    recording = true;
  }
  // The frames from start_recording's position on; *from is that position.
  std::vector<std::array<int32_t, 2>> stop_recording(uint64_t* from) {
    std::lock_guard lock(rec_m);
    recording = false;
    *from = rec_from;
    return std::move(rec);
  }
};

// The FLAC stream a server connection carries, decoded.
Decoded decode_conn(FakeIcecast& server, FakeIcecast::Conn& c) {
  const auto ogg = last_ogg_stream(server.body(c));
  REQUIRE(!ogg.empty());
  Decoded d = decode_ogg_flac(ogg);
  CHECK_FALSE(d.error);
  return d;
}

bool frame_is(const Decoded& d, size_t n, uint64_t t, const SampleFn& fn) {
  return d.samples[n * 2] == fn(t, 0) && d.samples[n * 2 + 1] == fn(t, 1);
}

// The media sample at which decoded frames [n0, n0 + len) start, when they equal fn bit for bit
// there; candidates [lo, hi) (the noise in the signals makes a match unique).
std::optional<uint64_t> find_start(const Decoded& d, size_t n0, size_t len, const SampleFn& fn, uint64_t lo, uint64_t hi) {
  REQUIRE(d.samples.size() / 2 >= n0 + len);
  for (uint64_t c = lo; c < hi; ++c) {
    if (!frame_is(d, n0, c, fn)) continue;
    bool all = true;
    for (size_t i = 1; i < len && all; ++i) all = frame_is(d, n0 + i, c + i, fn);
    if (all) return c;
  }
  return std::nullopt;
}

int32_t sig_dc(uint64_t, int) { return 1 << 30; }  // +0.5 full scale: 16384 in 16 bit

}  // namespace

TEST_CASE("encoder engine: AES67 -> 2 s delay + second input at -6 dB -> FLAC 48 kHz -> Icecast, bit for bit") {
  FakeIcecast server;
  const std::string kPort = "15220";
  const uint16_t rtp_port = 15220;
  const std::string group_a = "239.69.220.1", group_b = "239.69.220.2";  // same port: dispatch by group

  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig a;
  a.sdp = make_sdp("Wejście A", group_a, rtp_port);
  a.delay_s = 2.0;
  a.latency_us = 20000;
  be::InputConfig b;
  b.sdp = make_sdp("Wejście B", group_b, rtp_port);
  b.gain_db = kGainBDb;
  b.latency_us = 20000;
  st.inputs = {a, b};
  st.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/e2e.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender send_a(engine, group_a, rtp_port, sig_a);
  AesSender send_b(engine, group_b, rtp_port, sig_b);

  // Run until the station has mixed well past the 2 s delay (6.5 s of input on the media clock)
  // and the server has more than a second of the stream.
  REQUIRE(wait_for([&] { return send_a.first_ts != 0 && send_b.first_ts != 0; }, 5000));
  const uint64_t inputs_from = std::max(send_a.first_ts.load(), send_b.first_ts.load());
  const bool ran = wait_for(
      [&] {
        auto c = server.latest("/e2e.ogg");
        return c && c->bytes > 400'000 && engine.media_clock().sample_at(bp::monotonic_ns()) > inputs_from + 6 * 48000 + 24000;
      },
      30000);
  const be::EncoderStatus status = engine.status();
  auto conn = server.latest("/e2e.ogg");
  REQUIRE_MESSAGE(ran, "stream bytes at the server: " << (conn ? conn->bytes.load() : 0));
  const uint64_t now = engine.media_clock().sample_at(bp::monotonic_ns());

  // status: inputs receiving, station level, the stream on air
  REQUIRE(status.running);
  REQUIRE(status.stations.size() == 1);
  const auto& ss = status.stations[0];
  CHECK(ss.running);
  REQUIRE(ss.inputs.size() == 2);
  for (const auto& in : ss.inputs) {
    CHECK_MESSAGE(in.error.empty(), in.error);
    CHECK(in.signal);
    CHECK(in.rx.packets > 1000);
    CHECK(in.peak_db[0] > -20.0f);
  }
  CHECK(ss.inputs[0].name == "Wejście A");
  CHECK(ss.inputs[0].address == group_a + ":" + kPort);
  CHECK(ss.peak_db[0] > -20.0f);
  REQUIRE(ss.streams.size() == 1);
  CHECK(ss.streams[0].icecast.state == be::IcecastState::Streaming);
  CHECK(ss.streams[0].icecast.bytes_sent > 400'000);
  CHECK(ss.streams[0].encoder_error.empty());
  CHECK(ss.streams[0].codec_available);
  CHECK(ss.streams[0].queue_dropped_frames == 0);

  // decode the stream the server received
  const auto ogg = last_ogg_stream(server.body(*conn));
  REQUIRE(!ogg.empty());
  const Decoded dec = decode_ogg_flac(ogg);
  CHECK_FALSE(dec.error);
  CHECK(dec.sample_rate == 48000);
  CHECK(dec.channels == 2);
  CHECK(dec.bits == 16);
  const size_t frames = dec.samples.size() / 2;
  REQUIRE(frames > 48000 + 4800);

  // The last decoded second: find the media-clock sample it starts at (the noise makes the
  // match unique), then it must equal the expected mix bit for bit.
  const size_t n0 = frames - 48000;
  const int32_t* d = dec.samples.data() + n0 * 2;
  std::optional<uint64_t> start;
  for (uint64_t c = inputs_from; c < now && !start; ++c) {
    if (expected16(c, 0) != d[0] || expected16(c, 1) != d[1]) continue;
    bool all = true;
    for (size_t i = 1; i < 48000 && all; ++i) all = expected16(c + i, 0) == d[i * 2] && expected16(c + i, 1) == d[i * 2 + 1];
    if (all) start = c;
  }
  if (!start) {
    // diagnostics: how far off is it, assuming the window ends where the station is now
    size_t zeros = 0;
    for (size_t i = 0; i < 48000 * 2; ++i) zeros += d[i] == 0;
    MESSAGE("no 1 s window of the expected mix found; zero samples in the window: " << zeros);
  }
  REQUIRE(start);
  // the window lies where both inputs were already sending, A seen through its 2 s delay
  CHECK(*start >= inputs_from + kDelayFrames + 4800);
  MESSAGE("decoded second starts at media sample " << *start << ", " << (now - *start) << " frames before now");

  engine.stop();
  CHECK(wait_for([&] { return !conn->open; }, 5000));
  CHECK_FALSE(engine.status().running);
}

TEST_CASE("encoder engine: editing one stream restarts only that stream") {
  if (!be::codec_available(be::Codec::Mp3)) {
    MESSAGE("codec library for mp3 missing, skipped");
    return;
  }
  FakeIcecast server;
  server.refuse_busy = true;  // like Icecast: 403 for a second source on a mountpoint in use
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig in;
  in.sdp = make_sdp("Szum", "239.69.221.1", 15221);
  in.latency_us = 20000;
  st.inputs = {in};
  st.streams = {stream("str1", be::Codec::Mp3, 128, 44100, server.port(), "/a.mp3"),
                stream("str2", be::Codec::Flac, 0, 48000, server.port(), "/b.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender sender(engine, "239.69.221.1", 15221, sig_a);
  auto stream_status = [&](const std::string& id) -> std::optional<be::StreamStatus> {
    const be::EncoderStatus now = engine.status();
    if (now.stations.empty()) return std::nullopt;
    for (const auto& s : now.stations[0].streams)
      if (s.id == id) return s;
    return std::nullopt;
  };
  auto streaming = [&](const std::string& id) {
    const auto s = stream_status(id);
    return s && s->icecast.state == be::IcecastState::Streaming && s->icecast.bytes_sent > 0;
  };
  REQUIRE(wait_for([&] { return streaming("str1") && streaming("str2"); }, 10000));
  auto a1 = server.latest("/a.mp3");
  auto b1 = server.latest("/b.ogg");
  REQUIRE(a1);
  REQUIRE(b1);
  REQUIRE_MESSAGE(wait_for([&] { return a1->bytes > 1000 && b1->bytes > 1000; }, 10000),
                  "a: " << a1->bytes.load() << " bytes, b: " << b1->bytes.load() << " bytes");
  REQUIRE(wait_for([&] { return sender.first_ts != 0; }, 5000));
  const uint64_t inputs_from = sender.first_ts;
  const be::EncoderStatus before = engine.status();
  CHECK_FALSE(before.stations[0].streams[0].codec_library.empty());  // libmp3lame's path
  CHECK(before.stations[0].streams[1].codec_library.empty());        // FLAC is built in

  // str1: 128 -> 192 kb/s; str2: only its gain changes; the input: gain and delay (all live)
  cfg.stations[0].streams[0].bitrate_kbps = 192;
  cfg.stations[0].streams[1].gain_db = -3.0;
  cfg.stations[0].inputs[0].gain_db = -10.0;
  cfg.stations[0].inputs[0].delay_s = 0.5;
  // the same save also adds a station with three inputs (3 x 23 MB of delay lines to build):
  // the edited stream must not reconnect while its old source still holds the mountpoint
  be::StationConfig s2;
  s2.id = "s2";
  s2.name = "Radio Druga";
  for (int i = 0; i < 3; ++i) {
    be::InputConfig x;
    x.sdp = make_sdp("Druga " + std::to_string(i), "239.69.226." + std::to_string(i + 1), 15226);
    s2.inputs.push_back(x);
  }
  cfg.stations.push_back(s2);
  const be::EncoderConfig next = ready(cfg);
  const int64_t apply_from = bp::monotonic_ns();
  engine.apply_config(next);
  const int64_t apply_to = bp::monotonic_ns();
  const uint64_t changed_at = media_now(engine);

  // str1 comes back on a new connection without ever being refused (the old source went first)
  bool saw_error = false;
  REQUIRE(wait_for([&] {
    const auto s1 = stream_status("str1");
    saw_error = saw_error || (s1 && s1->icecast.state == be::IcecastState::Error);
    auto a2 = server.latest("/a.mp3");
    return a2 && a2->id != a1->id && a2->bytes > 1000;
  }, 10000));
  MESSAGE("apply took " << (apply_to - apply_from) / 1000 << " us; str1's new connection accepted "
          << (server.latest("/a.mp3")->accepted_ns - apply_from) / 1000 << " us after the apply began; refused " << server.refused.load());
  CHECK(server.refused == 0);
  CHECK_FALSE(saw_error);
  CHECK(stream_status("str1")->icecast.error.empty());
  CHECK(!a1->open);  // the old str1 connection was closed
  const size_t b_bytes = b1->bytes;
  CHECK(wait_for([&] { return b1->bytes > b_bytes + 1000; }, 5000));  // str2 keeps sending
  CHECK(b1->open);
  CHECK(server.latest("/b.ogg")->id == b1->id);  // on the same connection
  const be::EncoderStatus after = engine.status();
  CHECK(after.stations[0].started_ns == before.stations[0].started_ns);                // station not restarted
  CHECK(after.stations[0].streams[1].started_ns == before.stations[0].streams[1].started_ns);  // str2 neither
  CHECK(after.stations[0].streams[0].started_ns != before.stations[0].streams[0].started_ns);
  CHECK(after.stations[0].streams[0].bitrate_kbps == 192);
  CHECK_MESSAGE(after.stations[0].streams[1].icecast.reconnects == 0, "str2: " << after.stations[0].streams[1].icecast.error);
  // the input kept its socket and RxStream
  CHECK(after.stations[0].inputs[0].receiver_id == before.stations[0].inputs[0].receiver_id);
  CHECK(after.stations[0].inputs[0].rx.packets > before.stations[0].inputs[0].rx.packets);

  // the changes took effect: str2 now carries the input 0.5 s later at -10 dB, times -3 dB
  REQUIRE(wait_for([&] { return media_now(engine) > changed_at + 2 * 48000 + 24000; }, 10000));
  const Decoded dec = decode_conn(server, *b1);
  const size_t frames = dec.samples.size() / 2;
  REQUIRE(frames > 48000 * 2);
  const float g_in = float(std::pow(10.0, -10.0 / 20.0)), g_str = float(std::pow(10.0, -3.0 / 20.0));
  const SampleFn changed = [&](uint64_t t, int ch) { return pcm16(to_float(sig_a(t - 24000, ch)) * g_in * g_str); };
  const auto start = find_start(dec, frames - 48000, 48000, changed, inputs_from, media_now(engine));
  REQUIRE_MESSAGE(start, "the last second of str2 is not the input delayed 0.5 s at -13 dB");
  CHECK(*start >= changed_at);

  // disabling the station closes both connections
  cfg.stations[0].enabled = false;
  engine.apply_config(ready(cfg));
  CHECK(wait_for([&] {
    const auto a = server.latest("/a.mp3");
    return !b1->open && a && !a->open;
  }, 5000));
  const be::EncoderStatus off = engine.status();
  REQUIRE(off.stations.size() == 2);
  CHECK_FALSE(off.stations[0].running);
  CHECK(off.stations[1].running);
  CHECK(off.stations[0].streams[0].icecast.state == be::IcecastState::Idle);
}

TEST_CASE("encoder engine: deleting the first of two inputs leaves the second one as it was") {
  FakeIcecast server;
  const uint16_t port = 15223;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig a;
  a.sdp = make_sdp("A", "239.69.223.1", port);
  a.delay_s = 1.0;
  a.latency_us = 20000;
  be::InputConfig b;
  b.sdp = make_sdp("B", "239.69.223.2", port);
  b.latency_us = 20000;
  st.inputs = {a, b};
  st.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/d.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  const int base_rx = be::detail::testing::live_input_receivers();
  const int base_lines = be::detail::testing::live_delay_lines();
  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender send_a(engine, "239.69.223.1", port, sig_a);
  AesSender send_b(engine, "239.69.223.2", port, sig_b);
  REQUIRE(wait_for([&] { return send_a.first_ts != 0 && send_b.first_ts != 0; }, 5000));
  const uint64_t inputs_from = std::max(send_a.first_ts.load(), send_b.first_ts.load());
  REQUIRE(wait_for([&] {
    auto c = server.latest("/d.ogg");
    return c && c->bytes > 200'000 && media_now(engine) > inputs_from + 3 * 48000;
  }, 30000));
  auto conn = server.latest("/d.ogg");
  CHECK(be::detail::testing::live_input_receivers() == base_rx + 2);
  CHECK(be::detail::testing::live_delay_lines() == base_lines + 2);
  const be::EncoderStatus st0 = engine.status();
  const uint64_t b_receiver = st0.stations[0].inputs[1].receiver_id;
  const size_t frames_before = decode_conn(server, *conn).samples.size() / 2;
  REQUIRE(frames_before > 48000);

  cfg.stations[0].inputs.erase(cfg.stations[0].inputs.begin());  // delete A
  engine.apply_config(ready(cfg));
  const uint64_t changed_at = media_now(engine);
  // without any further API call, A's socket and its 23 MB delay line go within about a second
  CHECK(wait_for([&] {
    return be::detail::testing::live_input_receivers() == base_rx + 1 && be::detail::testing::live_delay_lines() == base_lines + 1;
  }, 2000));

  REQUIRE(wait_for([&] { return media_now(engine) > changed_at + 2 * 48000; }, 10000));
  const be::EncoderStatus st1 = engine.status();
  REQUIRE(st1.stations[0].inputs.size() == 1);
  CHECK(st1.stations[0].inputs[0].receiver_id == b_receiver);  // B kept its socket and RxStream
  CHECK(st1.stations[0].inputs[0].signal);
  CHECK(server.latest("/d.ogg")->id == conn->id);
  const Decoded dec = decode_conn(server, *conn);
  const size_t frames = dec.samples.size() / 2;

  const SampleFn both = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t - 48000, ch)) + to_float(sig_b(t, ch))); };
  const SampleFn only_b = [](uint64_t t, int ch) { return pcm16(to_float(sig_b(t, ch))); };
  const size_t n0 = frames_before - 24000;  // before the change: A (delayed 1 s) + B
  const auto t0 = find_start(dec, n0, 4800, both, inputs_from, changed_at);
  REQUIRE_MESSAGE(t0, "no window of A + B before the change");
  CHECK(*t0 >= inputs_from + 48000 + 4800);
  auto t = [&](size_t n) { return *t0 + (n - n0); };
  size_t n = n0;
  while (n < frames && frame_is(dec, n, t(n), both)) ++n;
  const size_t ab_end = n;
  CHECK(ab_end - n0 >= 4800);
  // then A fades out (at most 240 frames) and B goes on alone, bit for bit, with no gap
  size_t b_from = ab_end;
  for (size_t run = 0; b_from + run < frames && run < 16;) {
    if (frame_is(dec, b_from + run, t(b_from + run), only_b)) {
      ++run;
    } else {
      b_from += run + 1;
      run = 0;
    }
  }
  MESSAGE("A + B until frame " << ab_end << ", fade " << (b_from - ab_end) << " frames, B alone for " << (frames - b_from));
  CHECK(b_from - ab_end <= 240);
  size_t wrong = 0;
  for (size_t m = b_from; m < frames; ++m) wrong += !frame_is(dec, m, t(m), only_b);
  CHECK(wrong == 0);
  CHECK(frames - b_from >= 48000);
}

TEST_CASE("encoder engine: a new SDP on an input fades the old source out") {
  FakeIcecast server;
  const uint16_t port = 15224;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig a;
  a.sdp = make_sdp("DC", "239.69.224.1", port);
  a.delay_s = 0.5;
  a.latency_us = 20000;
  st.inputs = {a};
  st.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/e.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender dc(engine, "239.69.224.1", port, sig_dc);
  REQUIRE(wait_for([&] { return dc.first_ts != 0; }, 5000));
  REQUIRE(wait_for([&] {
    auto c = server.latest("/e.ogg");
    return c && media_now(engine) > dc.first_ts + 3 * 48000;
  }, 30000));
  auto conn = server.latest("/e.ogg");
  const uint64_t receiver = engine.status().stations[0].inputs[0].receiver_id;

  cfg.stations[0].inputs[0].sdp = make_sdp("Cisza", "239.69.224.2", port);  // nobody sends there
  engine.apply_config(ready(cfg));
  const uint64_t changed_at = media_now(engine);
  REQUIRE(wait_for([&] { return media_now(engine) > changed_at + 2 * 48000; }, 10000));
  CHECK(engine.status().stations[0].inputs[0].receiver_id != receiver);  // a new socket for the new SDP
  CHECK(server.latest("/e.ogg")->id == conn->id);

  const Decoded dec = decode_conn(server, *conn);
  const int32_t* d = dec.samples.data();
  const size_t frames = dec.samples.size() / 2;
  // the steady DC (after the 0.5 s delay filled), then the switch, then silence
  size_t steady = 0;
  for (size_t n = 0, run = 0; n < frames && !steady; ++n) {
    run = (d[n * 2] == 16384 && d[n * 2 + 1] == 16384) ? run + 1 : 0;
    if (run == 24000) steady = n + 1 - run;
  }
  REQUIRE_MESSAGE(steady, "no steady DC in the stream");
  int max_step = 0;
  size_t last_dc = steady;
  for (size_t n = steady + 1; n < frames; ++n) {
    for (int ch = 0; ch < 2; ++ch) max_step = std::max(max_step, std::abs(d[n * 2 + ch] - d[(n - 1) * 2 + ch]));
    if (d[n * 2] == 16384) last_dc = n;
  }
  size_t zeros = 0;
  for (size_t n = frames; n > 0 && d[(n - 1) * 2] == 0 && d[(n - 1) * 2 + 1] == 0; --n) ++zeros;
  MESSAGE("largest step after the DC settled: " << max_step << " LSB; silence at the end: " << zeros << " frames");
  CHECK(max_step <= 32768 / 240 + 1);  // a 5 ms ramp, never a cut
  CHECK(zeros >= 48000);               // nothing of the old source (or its delay line) afterwards
  CHECK(frames - zeros - last_dc <= 241);
}

TEST_CASE("encoder engine: a latency edit keeps the input and its 10 s of delay history") {
  FakeIcecast server;
  const uint16_t port = 15227;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig a;
  a.sdp = make_sdp("A", "239.69.227.1", port);
  a.delay_s = 10.0;
  a.latency_us = 5000;
  st.inputs = {a};
  st.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/f.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender sender(engine, "239.69.227.1", port, sig_a);
  REQUIRE(wait_for([&] { return sender.first_ts != 0; }, 5000));
  const uint64_t inputs_from = sender.first_ts;
  // until the 10 s delay has filled and the delayed input has been on air for a while
  REQUIRE(wait_for([&] { return server.latest("/f.ogg") && media_now(engine) > inputs_from + 12 * 48000; }, 40000));
  auto conn = server.latest("/f.ogg");
  const uint64_t receiver = engine.status().stations[0].inputs[0].receiver_id;

  cfg.stations[0].inputs[0].latency_us = 8000;  // 5 -> 8 ms
  engine.apply_config(ready(cfg));
  const uint64_t changed_at = media_now(engine);
  REQUIRE(wait_for([&] { return media_now(engine) > changed_at + 48000 + 24000; }, 10000));
  const be::EncoderStatus after = engine.status();
  CHECK(after.stations[0].inputs[0].receiver_id == receiver);  // the same socket and RxStream
  CHECK(after.stations[0].inputs[0].latency_us == 8000);
  CHECK(server.latest("/f.ogg")->id == conn->id);

  // the last 2 s of the stream, the edit in the middle: the input 10 s late, bit for bit,
  // with no hole and no step
  const Decoded dec = decode_conn(server, *conn);
  const size_t frames = dec.samples.size() / 2;
  REQUIRE(frames > 2 * 48000);
  const SampleFn delayed = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t - 480000, ch))); };
  const size_t n0 = frames - 2 * 48000;
  const auto t0 = find_start(dec, n0, 4800, delayed, inputs_from, changed_at);
  REQUIRE_MESSAGE(t0, "the stream 2 s before its end is not the input delayed by 10 s");
  size_t wrong = 0, first_wrong = 0;
  for (size_t n = n0; n < frames; ++n)
    if (!frame_is(dec, n, *t0 + (n - n0), delayed) && wrong++ == 0) first_wrong = n;
  if (wrong) MESSAGE("frames not matching around the latency edit: " << wrong << ", the first at window frame " << (first_wrong - n0));
  CHECK(wrong == 0);
  CHECK(*t0 < changed_at);                   // the window starts before the edit ...
  CHECK(*t0 + (frames - n0) > changed_at + 24000);  // ... and ends well after it
}

TEST_CASE("encoder engine: an input whose socket failed is tried again") {
  struct Restore {
    ~Restore() {
      be::detail::testing::fail_next_input_opens(0);
      be::detail::testing::set_input_retry_ms(0);
    }
  } restore;
  be::detail::testing::fail_next_input_opens(1);
  be::detail::testing::set_input_retry_ms(300);
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig in;
  in.sdp = make_sdp("Szum", "239.69.225.1", 15225);
  in.latency_us = 20000;
  st.inputs = {in};
  cfg.stations = {st};
  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(ready(cfg), &err), err);
  const be::InputStatus first = engine.status().stations.at(0).inputs.at(0);
  CHECK(first.error.find("gniazdo odbiorcze") != std::string::npos);
  CHECK_FALSE(first.signal);
  AesSender sender(engine, "239.69.225.1", 15225, sig_a);
  CHECK(wait_for([&] {
    const auto in = engine.status().stations.at(0).inputs.at(0);
    return in.error.empty() && in.signal;
  }, 5000));
  CHECK(engine.status().stations[0].inputs[0].receiver_id != first.receiver_id);
}

TEST_CASE("encoder engine: the automatic network card is detected again while running") {
  // At boot the card may have no address yet (or an APIPA one): the engine looks again about
  // every 2 s and moves inputs (and PTP) to the card once it is there. Loopback only: the
  // "detected" cards are 127.0.0.2 (boot) and 127.0.0.1.
  struct Detected {
    std::mutex m;
    std::string ip = "127.0.0.2";
    std::string get() {
      std::lock_guard l(m);
      return ip;
    }
    void set(std::string v) {
      std::lock_guard l(m);
      ip = std::move(v);
    }
  };
  static Detected detected;
  detected.set("127.0.0.2");
  struct Restore {
    ~Restore() { be::detail::testing::set_default_interface_hook({}); }
  } restore;
  be::detail::testing::set_default_interface_hook([] { return detected.get(); });

  FakeIcecast server;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = "";  // automatic
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig in;
  in.sdp = make_sdp("A", "239.69.231.1", 15231);
  in.latency_us = 20000;
  st.inputs = {in};
  st.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/g.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  CHECK(engine.status().interface_ip == "127.0.0.2");
  REQUIRE(wait_for([&] { return server.latest("/g.ogg") != nullptr; }, 10000));
  auto conn = server.latest("/g.ogg");
  const uint64_t receiver = engine.status().stations.at(0).inputs.at(0).receiver_id;

  // no card found for a while: everything stays as it is
  detected.set("");
  bp::sleep_ns(2'500'000'000);
  CHECK(engine.status().interface_ip == "127.0.0.2");
  CHECK(engine.status().stations[0].inputs[0].receiver_id == receiver);

  detected.set(kLoopback);
  AesSender sender(engine, "239.69.231.1", 15231, sig_a);
  CHECK(wait_for([&] { return engine.status().interface_ip == kLoopback; }, 5000));
  CHECK(wait_for([&] {
    const auto s = engine.status().stations.at(0).inputs.at(0);
    return s.receiver_id != receiver && s.error.empty() && s.signal;
  }, 5000));
  CHECK(engine.config().interface_ip.empty());  // still automatic
  CHECK(server.latest("/g.ogg")->id == conn->id);  // the stream kept its connection
}

TEST_CASE("encoder engine: old receivers are freed off the network thread") {
  // Closing a socket (IGMP leave) and freeing its rings must not run on the real-time network
  // thread, even when the network thread held the last receive set that had them.
  const int base_freed = be::detail::testing::input_receivers_freed_on_rx_thread();
  const int base_rx = be::detail::testing::live_input_receivers();
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig in;
  in.sdp = make_sdp("A", "239.69.232.1", 15232);
  in.latency_us = 20000;
  st.inputs = {in};
  cfg.stations = {st};
  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(ready(cfg), &err), err);
  for (int i = 0; i < 30; ++i) {
    cfg.stations[0].inputs[0].sdp = make_sdp("A", "239.69.232." + std::to_string(i % 3 + 1), 15232);
    engine.apply_config(ready(cfg));
    bp::sleep_ns(40'000'000);
  }
  // every replaced receiver goes once it has faded out and stopped lingering
  CHECK(wait_for([&] { return be::detail::testing::live_input_receivers() == base_rx + 1; }, 5000));
  engine.stop();
  CHECK(be::detail::testing::live_input_receivers() == base_rx);
  CHECK(be::detail::testing::input_receivers_freed_on_rx_thread() == base_freed);
}

TEST_CASE("encoder engine: a stream whose codec library is missing says so and runs nothing") {
  struct RestoreDefault {
    ~RestoreDefault() { be::set_codec_library_dirs({}); }  // back to the executable directory
  } restore;
  be::set_codec_library_dirs({std::string(BAM_TEST_DATA) + "/no-codecs-here"});
  REQUIRE_FALSE(be::codec_available(be::Codec::Mp3));

  FakeIcecast server;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  be::InputConfig in;
  in.sdp = make_sdp("Szum", "239.69.222.1", 15222);
  in.latency_us = 20000;
  st.inputs = {in};
  st.streams = {stream("str1", be::Codec::Mp3, 128, 44100, server.port(), "/a.mp3"),
                stream("str2", be::Codec::Flac, 0, 48000, server.port(), "/b.ogg")};
  cfg.stations = {st};
  cfg = ready(cfg);

  be::EncoderEngine engine(no_ptp());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender sender(engine, "239.69.222.1", 15222, sig_a);
  REQUIRE(wait_for([&] {
    const auto b = server.latest("/b.ogg");
    return b && b->bytes > 10000;
  }, 10000));
  bp::sleep_ns(6'000'000'000);  // longer than the 5 s queue: a fed but idle stream would drop
  const be::EncoderStatus s = engine.status();
  const auto& mp3 = s.stations.at(0).streams.at(0);
  CHECK_FALSE(mp3.codec_available);
  CHECK(mp3.codec_library.empty());
  CHECK(mp3.encoder_error.find("niedostępny") != std::string::npos);
  CHECK(mp3.icecast.state == be::IcecastState::Idle);
  CHECK(mp3.queue_dropped_frames == 0);  // not fed
  CHECK(server.latest("/a.mp3") == nullptr);  // never connected
  CHECK(s.stations[0].streams[1].icecast.state == be::IcecastState::Streaming);
}

TEST_CASE("encoder engine: the stream queue keeps the newest frames and counts what it drops") {
  be::detail::AudioQueue q(8);
  auto block = [](float first, size_t frames) {
    std::vector<float> v(frames * 2);
    for (size_t f = 0; f < frames; ++f) v[f * 2] = v[f * 2 + 1] = first + float(f);
    return v;
  };
  std::vector<float> out(16 * 2);
  CHECK(q.pop(out.data(), 16, 0) == 0);  // empty: nothing, no wait
  q.push(block(0, 5).data(), 5);
  q.push(block(5, 5).data(), 5);  // 10 > 8: the 2 oldest frames go
  CHECK(q.dropped_frames() == 2);
  REQUIRE(q.pop(out.data(), 3, 0) == 3);
  CHECK(out[0] == 2.0f);
  CHECK(out[5] == 4.0f);
  q.push(block(10, 3).data(), 3);  // wraps around the ring end
  REQUIRE(q.pop(out.data(), 16, 0) == 8);
  for (size_t f = 0; f < 8; ++f) {
    CHECK(out[f * 2] == float(5 + f));
    CHECK(out[f * 2 + 1] == float(5 + f));
  }
  q.push(block(100, 20).data(), 20);  // larger than the queue: only the newest 8 stay
  CHECK(q.dropped_frames() == 2 + 12);
  REQUIRE(q.pop(out.data(), 16, 0) == 8);
  CHECK(out[0] == 112.0f);
  CHECK(out[14] == 119.0f);
  // a waiting pop returns once woken
  std::thread waker([&] {
    bp::sleep_ns(50'000'000);
    q.wake();
  });
  const int64_t t0 = bp::monotonic_ns();
  CHECK(q.pop(out.data(), 16, 10000) == 0);
  CHECK(bp::monotonic_ns() - t0 < 5'000'000'000);
  waker.join();
  CHECK(be::detail::StreamWorker::kQueueFrames == 5 * 48000);  // spec 4.4: 5 s
}

TEST_CASE("encoder engine: with positions the queue never pops across a jump and keeps them across drops") {
  be::detail::AudioQueue q(8);
  std::vector<float> in(20 * 2, 0.0f), out(16 * 2);
  uint64_t pos = 0;
  q.push(in.data(), 3, 1000);
  q.push(in.data(), 3, 1003);  // follows on
  q.push(in.data(), 2, 5000);  // a jump
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 6);
  CHECK(pos == 1000);
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 2);
  CHECK(pos == 5000);
  q.push(in.data(), 4, 200);
  q.push(in.data(), 4, 300);
  q.push(in.data(), 3, 204);  // 11 > 8: the 3 oldest (200..202) go
  CHECK(q.dropped_frames() == 3);
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 1);
  CHECK(pos == 203);
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 4);
  CHECK(pos == 300);
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 3);
  CHECK(pos == 204);
  q.push(in.data(), 2, 10);
  q.push(in.data(), 20, 50);  // larger than the queue: only 62..69 stay
  CHECK(q.dropped_frames() == 3 + 12 + 2);
  REQUIRE(q.pop(out.data(), 16, 0, &pos) == 8);
  CHECK(pos == 62);
  // more jumps waiting than it keeps: the frames before the oldest one go
  be::detail::AudioQueue many(100);
  for (uint64_t k = 0; k < 40; ++k) many.push(in.data(), 1, k * 10);
  const uint64_t dropped = many.dropped_frames();
  CHECK(dropped > 0);
  for (uint64_t k = dropped; k < 40; ++k) {
    REQUIRE(many.pop(out.data(), 16, 0, &pos) == 1);
    CHECK(pos == k * 10);
  }
}

namespace {

// A BounceAES Master engine on 127.0.0.1 as the PTP master, on a domain nothing else uses.
struct LoopbackPtpMaster {
  std::string suffix = std::to_string(bp::current_pid()) + "." + std::to_string(bp::monotonic_ns() % 100000);
  bam::EngineOptions opts;
  std::unique_ptr<bam::Engine> engine;
  explicit LoopbackPtpMaster(int domain) {
    opts.config_path = "bam_enc_ptp_" + suffix + ".json";
    opts.shm_name = "bamtest.encptp." + suffix;
    opts.event_name = "bamtest.encptpev." + suffix;
    opts.enable_sap = false;
    auto cfg = bam::default_config();
    cfg.interface_ip = kLoopback;
    cfg.station_name = "PTP";
    cfg.ptp_domain = domain;
    cfg.ptp_role = bam::PtpRole::Preferred;
    std::ofstream(opts.config_path) << bam::config_to_json(cfg);
    engine = std::make_unique<bam::Engine>(opts);
  }
  ~LoopbackPtpMaster() {
    engine->stop();
    std::remove(opts.config_path.c_str());
    std::remove((opts.config_path + ".v1.bak").c_str());
    bp::SharedMemory::remove(opts.shm_name);
    bp::NamedEvent::remove(opts.event_name);
  }
};

}  // namespace

TEST_CASE("encoder engine: the PTP slave follows a master on loopback") {
  constexpr int kDomain = 87;
  LoopbackPtpMaster master(kDomain);
  std::string err;
  REQUIRE_MESSAGE(master.engine->start(&err), err);

  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.ptp_domain = kDomain;
  be::EncoderEngineOptions opts;  // PTP on (the default), free-running 10 s off the master's time
  opts.free_run_offset_ns = 10'000'000'000;
  be::EncoderEngine engine(opts);
  REQUIRE_MESSAGE(engine.start(ready(cfg), &err), err);
  CHECK(engine.status().ptp_free_run);  // nothing followed yet

  const bool locked = wait_for(
      [&] {
        const auto st = engine.status();
        return st.ptp.state == bam::PtpState::Locked && !st.ptp_free_run;
      },
      40000);
  const be::EncoderStatus st = engine.status();
  REQUIRE_MESSAGE(locked, "state " << int(st.ptp.state) << ", error: " << st.ptp_error);
  CHECK(st.ptp_enabled);
  CHECK(st.ptp_error.empty());
  CHECK(st.ptp.has_master);
  CHECK(st.ptp.domain == kDomain);
  CHECK(st.ptp.grandmaster == master.engine->status().ptp.grandmaster);
  CHECK(std::llabs(st.ptp.offset_ns) < 1'000'000);
  CHECK_FALSE(st.ptp_free_run);  // free-run: not locked to a master right now
  // the media clock now follows the master's (10 s away before): "now" is the same sample within 1 ms
  const bam::MediaClock mine = engine.media_clock();
  std::optional<bp::SharedMemory> shm = bp::SharedMemory::open(master.opts.shm_name, bam::ShmLayout::total_bytes(), &err);
  REQUIRE_MESSAGE(shm, err);
  uint64_t generation = 0;
  const bam::MediaClock theirs = bam::ShmView(shm->data()).read_clock(&generation);
  const int64_t now = bp::monotonic_ns();
  const int64_t diff = int64_t(mine.sample_at(now)) - int64_t(theirs.sample_at(now));
  CHECK(std::llabs(diff) <= 48);
  engine.stop();
}

#ifdef BAM_FAKE_STEREOTOOL_PATH

// ---- Stereo Tool processing (the fake library: output = input delayed by `latency` x `gain`) ----

namespace {

namespace fs = std::filesystem;
using fake_st::fake;
using fake_st::UseFake;

constexpr const char* kKey1 = "<FAKE-LICENCE-KEY-0001>";  // test keys only, never a real one
constexpr const char* kKey2 = "<FAKE-LICENCE-KEY-0002>";

std::string fake_preset(double gain, int latency) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "[FakeStereoTool]\ngain=%.9g\nlatency=%d\n", gain, latency);
  return buf;
}

// The stereotool folder of a test engine.
struct StDir {
  std::string path;
  explicit StDir(const std::string& tag) {
    path = (fs::temp_directory_path() / ("bam_enc_st_" + tag + "_" + std::to_string(bp::current_pid()))).string();
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  ~StDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  StDir(const StDir&) = delete;
  StDir& operator=(const StDir&) = delete;
  std::string presets() const { return (fs::path(path) / "presets").string(); }
};

be::EncoderEngineOptions st_options(const StDir& d) {
  be::EncoderEngineOptions o = no_ptp();
  o.stereotool_dir = d.path;
  return o;
}

be::ProcessingConfig processing(const std::string& preset, const std::string& key = kKey1) {
  be::ProcessingConfig p;
  p.enabled = true;
  p.preset = preset;
  p.license_key = key;
  return p;
}

be::StationConfig station(const std::string& id, const std::string& name) {
  be::StationConfig st;
  st.id = id;
  st.name = name;
  return st;
}

be::ProcessingStatus processing_of(const be::EncoderEngine& e, const std::string& id) {
  for (const auto& st : e.status().stations)
    if (st.id == id) return st.processing;
  FAIL("no station " << id);
  return {};
}

bool processing_running(const be::EncoderEngine& e, const std::string& id, int timeout_ms = 10000) {
  return wait_for([&] { return processing_of(e, id).state == be::ProcessingState::Running; }, timeout_ms);
}

// Log lines (of the last 500) that contain `what`.
size_t log_lines_with(const std::string& what) {
  size_t n = 0;
  for (const auto& e : bam::Logger::instance().recent())
    if (e.message.find(what) != std::string::npos) ++n;
  return n;
}

bool logged(const std::string& what) { return log_lines_with(what) > 0; }

// The longest run of frames, after the first one above `limit`, with both sides within ±limit.
size_t longest_quiet(const Decoded& d, int limit) {
  const size_t frames = d.samples.size() / 2;
  auto quiet = [&](size_t n) { return std::abs(d.samples[n * 2]) <= limit && std::abs(d.samples[n * 2 + 1]) <= limit; };
  size_t n = 0;
  while (n < frames && quiet(n)) ++n;
  size_t longest = 0, run = 0;
  for (; n < frames; ++n) {
    run = quiet(n) ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  return longest;
}

// Delays apply_config right after a processing worker is built (as a slow log line would).
struct SlowBuild {
  explicit SlowBuild(int64_t ns) {
    be::detail::testing::set_processing_built_hook([ns] { bp::sleep_ns(ns); });
  }
  ~SlowBuild() { be::detail::testing::set_processing_built_hook({}); }
  SlowBuild(const SlowBuild&) = delete;
  SlowBuild& operator=(const SlowBuild&) = delete;
};

// Walks decoded frames from n0 (media sample t0) to the end: each frame must be `dry`, `proc`
// (= dry x gain in float) or a frame of a 240-frame crossfade between them computed as the
// worker does, with no frame skipped or repeated. Returns the mismatches; `switches` counts the
// crossfades.
size_t walk_path(const Decoded& d, size_t n0, uint64_t t0, float gain, int32_t (*sig)(uint64_t, int), size_t* switches) {
  const size_t frames = d.samples.size() / 2;
  auto x = [&](uint64_t t, int ch) { return to_float(sig(t, ch)); };
  auto dry = [&](uint64_t t, int ch) { return pcm16(x(t, ch)); };
  auto proc = [&](uint64_t t, int ch) { return pcm16(x(t, ch) * gain); };
  auto fade = [&](uint64_t t, int ch, uint32_t k, bool to_proc) {
    const double from = to_proc ? 0.0 : 1.0, target = to_proc ? 1.0 : 0.0;
    const uint32_t left = be::kRampFrames - k;
    const double a = left == 0 ? target : target + (from - target) * (double(left) / be::kRampFrames);
    return pcm16(static_cast<float>(a * double(x(t, ch) * gain) + (1.0 - a) * double(x(t, ch))));
  };
  auto is = [&](size_t n, auto&& fn) { return d.samples[n * 2] == fn(t0 + (n - n0), 0) && d.samples[n * 2 + 1] == fn(t0 + (n - n0), 1); };
  bool processed = false;
  size_t bad = 0;
  *switches = 0;
  for (size_t n = n0; n < frames;) {
    if (processed ? is(n, proc) : is(n, dry)) {
      ++n;
      continue;
    }
    const bool to = !processed;
    if (is(n, [&](uint64_t t, int ch) { return fade(t, ch, 1, to); })) {
      ++*switches;
      for (uint32_t k = 1; k <= be::kRampFrames && n < frames; ++k, ++n)
        bad += !is(n, [&](uint64_t t, int ch) { return fade(t, ch, k, to); });
      processed = to;
      continue;
    }
    ++bad;
    ++n;
  }
  return bad;
}

}  // namespace

TEST_CASE("encoder engine: a new processing worker passes on its first block (switch on, new instance)") {
  UseFake use;
  StDir dir("first");
  FakeIcecast server;
  const uint16_t port = 15243;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig s1 = station("s1", "Radio Pierwszy Blok");
  be::InputConfig in;
  in.sdp = make_sdp("A", "239.69.243.1", port);
  in.latency_us = 20000;
  s1.inputs = {in};
  s1.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/f.ogg")};
  s1.processing = processing("Half.sts");
  s1.processing.enabled = false;
  cfg.stations = {s1};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Half.sts", fake_preset(0.5, 512)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender sender(engine, "239.69.243.1", port, sig_a);
  REQUIRE(wait_for([&] { return sender.first_ts != 0; }, 5000));
  const uint64_t inputs_from = sender.first_ts;
  auto on_air_for = [&](uint64_t frames) {
    const uint64_t from = media_now(engine);
    REQUIRE(wait_for([&] { return media_now(engine) > from + frames; }, 20000));
  };
  on_air_for(48000 + 24000);
  auto conn = server.latest("/f.ogg");
  REQUIRE(conn);
  const size_t before = decode_conn(server, *conn).samples.size() / 2;
  REQUIRE(before > 24000);

  SlowBuild slow(5'000'000);  // like one slow log line: the worker's output thread is waiting for audio before it gets its streams
  cfg.stations[0].processing.enabled = true;
  engine.apply_config(ready(cfg));
  REQUIRE(processing_running(engine, "s1"));
  on_air_for(48000);
  // every frame from before the switch to the end: the input, then (crossfaded) the input at -6 dB
  const Decoded dec = decode_conn(server, *conn);
  const size_t n0 = before - 9600;
  const SampleFn dry = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t, ch))); };
  const auto t0 = find_start(dec, n0, 4800, dry, inputs_from, media_now(engine));
  REQUIRE_MESSAGE(t0, "no window of the unprocessed input before the switch");
  size_t switches = 0;
  const size_t bad = walk_path(dec, n0, *t0, 0.5f, sig_a, &switches);
  CHECK_MESSAGE(bad == 0, "frames not on the path (a lost or repeated block): " << bad);
  CHECK(switches == 1);
  CHECK(server.latest("/f.ogg")->id == conn->id);
}

TEST_CASE("encoder engine: a station through Stereo Tool, bit for bit; a preset change and switching it off and on never silence it") {
  UseFake use;
  StDir dir("e2e");
  FakeIcecast server;
  const uint16_t rtp_port = 15240;
  const std::string group_a = "239.69.240.1", group_b = "239.69.240.2";
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig s1 = station("s1", "Radio Processing");
  be::InputConfig a;
  a.sdp = make_sdp("A", group_a, rtp_port);
  a.latency_us = 20000;
  s1.inputs = {a};
  s1.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/p.ogg")};
  s1.processing = processing("Fake -6 dB.sts");
  be::StationConfig s2 = station("s2", "Radio Czysta");  // no processing: the 1.0 path
  be::InputConfig b;
  b.sdp = make_sdp("B", group_b, rtp_port);
  b.latency_us = 20000;
  s2.inputs = {b};
  s2.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/c.ogg")};
  cfg.stations = {s1, s2};
  cfg = ready(cfg);

  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Fake -6 dB.sts", fake_preset(0.5, 512)).ok());  // before the first start too
  REQUIRE(engine.put_preset("Fake -12 dB.sts", fake_preset(0.25, 1024)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender send_a(engine, group_a, rtp_port, sig_a);
  AesSender send_b(engine, group_b, rtp_port, sig_b);
  REQUIRE(wait_for([&] { return send_a.first_ts != 0 && send_b.first_ts != 0; }, 5000));
  const uint64_t inputs_from = std::max(send_a.first_ts.load(), send_b.first_ts.load());

  const SampleFn proc6 = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t, ch)) * 0.5f); };
  const SampleFn proc12 = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t, ch)) * 0.25f); };
  const SampleFn dry = [](uint64_t t, int ch) { return pcm16(to_float(sig_a(t, ch))); };
  const SampleFn clean_b = [](uint64_t t, int ch) { return pcm16(to_float(sig_b(t, ch))); };
  // the last `len` frames a connection carries are `fn` bit for bit (somewhere on the media clock)
  auto tail_is = [&](FakeIcecast::Conn& c, const SampleFn& fn, size_t len) {
    const Decoded d = decode_conn(server, c);
    const size_t frames = d.samples.size() / 2;
    REQUIRE(frames >= len);
    return find_start(d, frames - len, len, fn, inputs_from, media_now(engine)).has_value();
  };
  auto on_air_for = [&](uint64_t frames) {
    const uint64_t from = media_now(engine);
    REQUIRE(wait_for([&] { return media_now(engine) > from + frames; }, 20000));
  };

  REQUIRE(processing_running(engine, "s1"));
  on_air_for(2 * 48000);
  auto conn_p = server.latest("/p.ogg");
  auto conn_c = server.latest("/c.ogg");
  REQUIRE(conn_p);
  REQUIRE(conn_c);
  const be::EncoderStatus st = engine.status();
  const be::ProcessingStatus& ps = st.stations.at(0).processing;
  CHECK(ps.state == be::ProcessingState::Running);
  CHECK(ps.reason_code == be::ProcessingReason::None);
  CHECK(ps.reason.empty());
  CHECK(ps.enabled);
  CHECK(ps.preset == "Fake -6 dB.sts");
  CHECK(ps.latency_ms == doctest::Approx((512.0 + 240.0) / 48.0));
  CHECK(ps.license_valid);
  CHECK(ps.web_port == be::kDefaultStereoToolWebPortBase);  // base + position 0
  CHECK(ps.retry_in_s == -1);
  CHECK(ps.queue_dropped_frames == 0);
  CHECK(st.stations.at(1).processing.state == be::ProcessingState::Off);
  CHECK_FALSE(st.stations.at(1).processing.enabled);
  CHECK(st.stereotool.available);
  CHECK(st.stereotool.version == 11070);
  CHECK(fake().text("create_key") == std::optional<std::string>(kKey1));
  CHECK(fake().text("create_name") == std::optional<std::string>("station-s1"));
  CHECK(fake().text("create_home") == std::optional<std::string>((fs::path(dir.path) / "station-s1").string()));
  CHECK(fake().text("load_path") == std::optional<std::string>((fs::path(dir.presets()) / "Fake -6 dB.sts").string()));
  CHECK(fake().sts("station-s1", be::detail::st::kParamWebserverPort) == std::optional<std::string>("8100"));
  CHECK(fake().value("live") == 1);  // one instance: the station without processing has none
  CHECK_MESSAGE(tail_is(*conn_p, proc6, 24000), "the processed station is not its input at -6 dB");
  CHECK_MESSAGE(tail_is(*conn_c, clean_b, 48000), "the station without processing is not its input");

  // another preset: loaded into the same instance (here with a longer latency)
  cfg.stations[0].processing.preset = "fake -12 DB.sts";  // any case
  engine.apply_config(ready(cfg));
  REQUIRE(wait_for([&] {
    const auto p = processing_of(engine, "s1");
    return p.state == be::ProcessingState::Running && p.latency_ms > 26.0;
  }, 10000));
  on_air_for(48000 + 24000);
  CHECK_MESSAGE(tail_is(*conn_p, proc12, 24000), "after the preset change the station is not its input at -12 dB");
  CHECK(fake().value("created") == 1);
  CHECK(fake().value("load_calls") == 2);
  CHECK(processing_of(engine, "s1").latency_ms == doctest::Approx((1024.0 + 240.0) / 48.0));

  // processing off: the instance goes, the station is on the 1.0 path again
  cfg.stations[0].processing.enabled = false;
  engine.apply_config(ready(cfg));
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Off);
  CHECK(wait_for([] { return fake().value("live") == 0; }, 5000));
  on_air_for(48000 + 24000);
  CHECK_MESSAGE(tail_is(*conn_p, dry, 48000), "with processing off the station is not its input");

  // and on again
  cfg.stations[0].processing.enabled = true;
  cfg.stations[0].processing.preset = "Fake -6 dB.sts";
  engine.apply_config(ready(cfg));
  REQUIRE(processing_running(engine, "s1"));
  on_air_for(48000 + 24000);
  CHECK_MESSAGE(tail_is(*conn_p, proc6, 24000), "processing on again: not the input at -6 dB");
  CHECK(fake().value("created") == 2);

  // one connection each all along, never a silent moment (the switches are 5 ms ramps)
  CHECK(server.latest("/p.ogg")->id == conn_p->id);
  CHECK(server.latest("/c.ogg")->id == conn_c->id);
  const Decoded dp = decode_conn(server, *conn_p);
  const size_t quiet = longest_quiet(dp, 16);
  MESSAGE("longest quiet run on the processed station: " << quiet << " frames");
  CHECK(quiet <= 8);
  CHECK_MESSAGE(tail_is(*conn_c, clean_b, 48000), "the station without processing changed");
  CHECK(longest_quiet(decode_conn(server, *conn_c), 16) <= 8);
  CHECK(logged("Processing stacji „Radio Processing”: Stereo Tool przetwarza sygnał"));
  CHECK(logged("Processing stacji „Radio Processing”: nowy preset „fake -12 DB.sts”"));
  CHECK(logged("Processing stacji „Radio Processing”: wyłączony"));
  CHECK(log_lines_with(kKey1) == 0);
}

TEST_CASE("encoder engine: switching processing off fades out and in, switching it on is seamless") {
  UseFake use;
  StDir dir("fade");
  FakeIcecast server;
  const uint16_t port = 15241;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig s1 = station("s1", "Radio Rampa");
  be::InputConfig in;
  in.sdp = make_sdp("DC", "239.69.241.1", port);
  in.latency_us = 20000;
  s1.inputs = {in};
  s1.streams = {stream("str1", be::Codec::Flac, 0, 48000, server.port(), "/r.ogg")};
  s1.processing = processing("Half.sts");
  cfg.stations = {s1};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Half.sts", fake_preset(0.5, 512)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender dc(engine, "239.69.241.1", port, sig_dc);  // 0.5 full scale: 16384, processed 8192
  auto on_air_for = [&](uint64_t frames) {
    const uint64_t from = media_now(engine);
    REQUIRE(wait_for([&] { return media_now(engine) > from + frames; }, 20000));
  };
  REQUIRE(processing_running(engine, "s1"));
  on_air_for(48000);
  SlowBuild slow(5'000'000);  // like one slow log line: a new worker's output thread waits for audio before it gets its streams
  cfg.stations[0].processing.enabled = false;  // fade out (worker) + fade in (station)
  engine.apply_config(ready(cfg));
  on_air_for(48000);
  cfg.stations[0].processing.enabled = true;  // seamless, then a crossfade once Stereo Tool is ready
  engine.apply_config(ready(cfg));
  REQUIRE(processing_running(engine, "s1"));
  on_air_for(48000);
  for (const char* key : {kKey2, kKey1, kKey2}) {  // a new instance each time: fade out, fade in
    cfg.stations[0].processing.license_key = key;
    engine.apply_config(ready(cfg));
    REQUIRE(processing_running(engine, "s1"));
    on_air_for(24000);
  }
  on_air_for(24000);
  CHECK(fake().value("created") == 5);
  CHECK(fake().text("create_key") == std::optional<std::string>(kKey2));
  CHECK(fake().value("live") == 1);

  auto conn = server.latest("/r.ogg");
  REQUIRE(conn);
  const Decoded dec = decode_conn(server, *conn);
  const int32_t* d = dec.samples.data();
  const size_t frames = dec.samples.size() / 2;
  size_t start = 0;
  while (start < frames && d[start * 2] != 16384) ++start;
  REQUIRE(start < frames);
  int max_step = 0;
  size_t zeros = 0, processed = 0, longest_zero = 0, run = 0;
  for (size_t n = start + 1; n < frames; ++n) {
    for (int ch = 0; ch < 2; ++ch) max_step = std::max(max_step, std::abs(d[n * 2 + ch] - d[(n - 1) * 2 + ch]));
    const bool zero = d[n * 2] == 0 && d[n * 2 + 1] == 0;
    zeros += zero;
    run = zero ? run + 1 : 0;
    longest_zero = std::max(longest_zero, run);
    processed += d[n * 2] == 8192;
  }
  MESSAGE("largest step " << max_step << " LSB, zero frames " << zeros << ", processed frames " << processed);
  CHECK(max_step <= 16384 / 240 + 2);  // ramps only, never a cut
  CHECK(longest_zero <= 2);            // the end of a fade out, then the fade in
  CHECK(zeros <= 8);                   // four switches away from a worker
  CHECK(processed > 48000);
  CHECK(server.latest("/r.ogg")->id == conn->id);
}

TEST_CASE("encoder engine: processing settings reach Stereo Tool; a new key or port builds a new instance") {
  UseFake use;
  StDir dir("settings");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stereotool_web_port_base = 8300;
  // no inputs: the stations mix silence, which Stereo Tool processes all the same
  be::StationConfig plain = station("s1", "Radio Bez");
  be::StationConfig proc = station("s2", "Radio Ustawienia");
  proc.processing = processing("Ustawienia.sts");
  cfg.stations = {plain, proc};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Ustawienia.sts", fake_preset(0.5, 256)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s2"));
  CHECK(processing_of(engine, "s2").web_port == 8301);  // base + the station's position
  CHECK(fake().sts("station-s2", be::detail::st::kParamWebserverPort) == std::optional<std::string>("8301"));
  CHECK(fake().sts("station-s2", be::detail::st::kParamWebserverEnable) == std::optional<std::string>("1"));
  CHECK(fs::is_directory(fs::path(dir.path) / "station-s2"));
  CHECK_FALSE(fs::exists(fs::path(dir.path) / "station-s1"));
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Off);
  CHECK(fake().value("created") == 1);

  SUBCASE("a new key") {
    cfg.stations[1].processing.license_key = kKey2;
    engine.apply_config(ready(cfg));
    REQUIRE(processing_running(engine, "s2"));
    CHECK(fake().value("created") == 2);
    CHECK(fake().text("create_key") == std::optional<std::string>(kKey2));
  }
  SUBCASE("the default key when the station has none") {
    cfg.stations[1].processing.license_key.clear();
    cfg.stereotool_default_key = kKey2;
    engine.apply_config(ready(cfg));
    REQUIRE(processing_running(engine, "s2"));
    CHECK(fake().value("created") == 2);
    CHECK(fake().text("create_key") == std::optional<std::string>(kKey2));
  }
  SUBCASE("another web port base") {
    cfg.stereotool_web_port_base = 8400;
    engine.apply_config(ready(cfg));
    REQUIRE(processing_running(engine, "s2"));
    CHECK(fake().value("created") == 2);
    CHECK(processing_of(engine, "s2").web_port == 8401);
  }
  SUBCASE("other stations moved or deleted: the port stays, so does the instance (ruling S3)") {
    std::swap(cfg.stations[0], cfg.stations[1]);
    engine.apply_config(ready(cfg));
    bp::sleep_ns(300'000'000);
    CHECK(processing_of(engine, "s2").web_port == 8301);
    cfg.stations.erase(cfg.stations.begin() + 1);  // the station that was before it
    be::StationConfig added = station("", "Radio Nowa");
    cfg.stations.push_back(added);
    cfg = ready(cfg);
    CHECK(cfg.stations[1].processing.web_port_slot == 0);  // the new one gets the free slot
    engine.apply_config(cfg);
    bp::sleep_ns(300'000'000);
    CHECK(processing_of(engine, "s2").state == be::ProcessingState::Running);
    CHECK(processing_of(engine, "s2").web_port == 8301);
    CHECK(fake().value("created") == 1);
  }
  SUBCASE("only a stream or input change keeps the instance") {
    be::InputConfig in;
    in.sdp = make_sdp("X", "239.69.242.1", 15242);
    cfg.stations[1].inputs = {in};
    engine.apply_config(ready(cfg));
    bp::sleep_ns(300'000'000);
    CHECK(processing_of(engine, "s2").state == be::ProcessingState::Running);
    CHECK(fake().value("created") == 1);
  }
  CHECK(fake().value("live") == 1);
  // the station stopped: its instance goes
  for (auto& st : cfg.stations)
    if (st.id == "s2") st.enabled = false;
  engine.apply_config(ready(cfg));
  CHECK(wait_for([] { return fake().value("live") == 0; }, 5000));
  CHECK(processing_of(engine, "s2").state == be::ProcessingState::Off);
  CHECK(processing_of(engine, "s2").enabled);  // still configured
}

TEST_CASE("encoder engine: the status does not load the Stereo Tool library when no station uses it") {
  UseFake use;
  StDir dir("peek");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {station("s1", "Radio Bez")};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  bp::sleep_ns(200'000'000);
  const be::EncoderStatus s = engine.status();
  CHECK_FALSE(s.stereotool.checked);
  CHECK_FALSE(s.stereotool.available);
  CHECK(fake().value("version_calls") == 0);
  CHECK(s.stations.at(0).processing.state == be::ProcessingState::Off);
  // a station with processing: the library is loaded for it
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  cfg.stations[0].processing = processing("P.sts");
  engine.apply_config(ready(cfg));
  REQUIRE(processing_running(engine, "s1"));
  const be::EncoderStatus s2 = engine.status();
  CHECK(s2.stereotool.checked);
  CHECK(s2.stereotool.available);
  CHECK(s2.stereotool.version == 11070);
  CHECK(s2.stereotool.path.find("fake_stereotool") != std::string::npos);
}

TEST_CASE("encoder engine: without a Stereo Tool folder processing is an error, never Stereo Tool's defaults") {
  UseFake use;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Bez Folderu");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(no_ptp());  // no stereotool_dir
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(wait_for([&] { return processing_of(engine, "s1").state == be::ProcessingState::Error; }, 5000));
  const auto p = processing_of(engine, "s1");
  CHECK(p.reason_code == be::ProcessingReason::Setup);
  CHECK_MESSAGE(p.reason.find("brak folderu presetów") != std::string::npos, p.reason);
  CHECK(fake().value("create_calls") == 0);
  CHECK(engine.list_presets().empty());
  CHECK(engine.put_preset("P.sts", "x").error == be::PresetError::Failed);
}

TEST_CASE("encoder engine: presets are stored with sanitised names and limits, in-use ones stay") {
  UseFake use;
  StDir dir("presets");
  const std::string data = fake_preset(0.5, 0);
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Presety");
  st.processing = processing("fm rock.sts");  // as the station refers to it (any case)
  st.enabled = false;  // a stopped station keeps its preset too
  cfg.stations = {st};
  cfg = ready(cfg);
  // left-overs of an upload that never finished, and a file that is not a preset
  fs::create_directories(dir.presets());
  std::ofstream(fs::path(dir.presets()) / ".bounce-7.tmp") << "half";
  std::ofstream(fs::path(dir.presets()) / "notatki.txt") << "x";
  be::EncoderEngine engine(st_options(dir));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  CHECK_FALSE(fs::exists(fs::path(dir.presets()) / ".bounce-7.tmp"));
  CHECK(engine.list_presets().empty());

  CHECK(engine.put_preset("FM Rock.sts", data).ok());
  CHECK(engine.put_preset("ambient-2.STS", data).ok());
  CHECK(engine.put_preset("b_news.sts", std::string("x")).ok());
  const auto list = engine.list_presets();
  REQUIRE(list.size() == 3);
  CHECK(list[0].name == "ambient-2.STS");  // by name, case-insensitively
  CHECK(list[1].name == "b_news.sts");
  CHECK(list[2].name == "FM Rock.sts");
  CHECK(list[2].bytes == data.size());
  std::string got;
  CHECK(engine.get_preset("fm ROCK.sts", &got).ok());
  CHECK(got == data);

  auto error_of = [](const be::PresetResult& r) { return r.error; };
  CHECK(error_of(engine.put_preset("fm rock.STS", data)) == be::PresetError::Exists);
  for (const char* bad : {"../x.sts", "a/b.sts", "a\\b.sts", "C:x.sts", "CON.sts", "x.txt", ".sts", " x.sts", "", "ż.sts"})
    CHECK_MESSAGE(error_of(engine.put_preset(bad, data)) == be::PresetError::BadName, bad);
  CHECK(error_of(engine.put_preset("empty.sts", "")) == be::PresetError::Empty);
  CHECK(error_of(engine.put_preset("big.sts", std::string(be::kMaxPresetBytes + 1, 'x'))) == be::PresetError::TooLarge);
  CHECK(engine.put_preset("max.sts", std::string(be::kMaxPresetBytes, 'x')).ok());
  CHECK(error_of(engine.get_preset("nie-ma.sts", &got)) == be::PresetError::NotFound);
  CHECK(error_of(engine.get_preset("../config.json", &got)) == be::PresetError::NotFound);
  CHECK(error_of(engine.remove_preset("nie-ma.sts")) == be::PresetError::NotFound);
  // in use by a station: refused, whatever the case
  const be::PresetResult in_use = engine.remove_preset("FM ROCK.sts");
  CHECK(in_use.error == be::PresetError::InUse);
  CHECK_MESSAGE(in_use.message.find("„Radio Presety”") != std::string::npos, in_use.message);
  CHECK(engine.list_presets().size() == 4);
  CHECK(engine.remove_preset("B_NEWS.sts").ok());
  CHECK_FALSE(fs::exists(fs::path(dir.presets()) / "b_news.sts"));
  // at most 64
  for (size_t i = engine.list_presets().size(); i < be::kMaxPresets; ++i)
    REQUIRE(engine.put_preset("p" + std::to_string(i) + ".sts", data).ok());
  CHECK(engine.list_presets().size() == be::kMaxPresets);
  CHECK(error_of(engine.put_preset("one-more.sts", data)) == be::PresetError::TooMany);
  // nothing but presets in the folder (no temporary file stays behind)
  size_t files = 0;
  for (const auto& e : fs::directory_iterator(dir.presets())) files += e.path().filename() != "notatki.txt";
  CHECK(files == be::kMaxPresets);
}

TEST_CASE("encoder engine: save the station's settings as a preset, restart processing") {
  UseFake use;
  StDir dir("save");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Zapis");
  st.processing = processing("Start.sts");
  cfg.stations = {st, station("s2", "Radio Bez")};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Start.sts", fake_preset(0.5, 128)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));

  const be::PresetResult saved = engine.save_processing_preset("s1", "Strojony.sts");
  REQUIRE_MESSAGE(saved.ok(), saved.message);
  std::string data;
  REQUIRE(engine.get_preset("strojony.sts", &data).ok());
  CHECK(data.rfind(fake_preset(0.5, 128), 0) == 0);  // what the instance runs with (then its .sts values)
  CHECK(fake().value("save_calls") == 1);
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Running);  // no bypass for a save
  CHECK(engine.save_processing_preset("s1", "STROJONY.sts").error == be::PresetError::Exists);
  CHECK(engine.save_processing_preset("s1", "../x.sts").error == be::PresetError::BadName);
  CHECK(engine.save_processing_preset("s2", "Inny.sts").error == be::PresetError::NotRunning);
  CHECK(engine.save_processing_preset("s9", "Inny.sts").error == be::PresetError::NotRunning);
  fake().set("save_fails", 1);
  const be::PresetResult failed = engine.save_processing_preset("s1", "Nieudany.sts");
  CHECK(failed.error == be::PresetError::Failed);
  CHECK_MESSAGE(failed.message.find("nie zapisał presetu") != std::string::npos, failed.message);
  CHECK(engine.list_presets().size() == 2);
  size_t files = 0;
  for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir.presets())) ++files;
  CHECK(files == 2);  // no temporary file left
  fake().set("save_fails", 0);
  CHECK(logged("Processing stacji „Radio Zapis”: ustawienia zapisane jako preset „Strojony.sts”"));

  // restart: a failed instance is built anew
  fake().set("process_throws", 1);
  REQUIRE(wait_for([&] { return processing_of(engine, "s1").reason_code == be::ProcessingReason::Process; }, 5000));
  fake().set("process_throws", 0);
  bp::sleep_ns(300'000'000);
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Bypass);  // not retried on its own
  REQUIRE(engine.restart_processing("s1", &err));
  REQUIRE(processing_running(engine, "s1"));
  CHECK(fake().value("created") == 2);
  CHECK_FALSE(engine.restart_processing("s2", &err));
  CHECK_MESSAGE(err.find("nie ma działającego processingu") != std::string::npos, err);
  CHECK(logged("Processing stacji „Radio Zapis”: ponowne uruchomienie (ręczne)"));
  CHECK(logged("Processing stacji „Radio Zapis”: Stereo Tool zgłosił wyjątek w stereoTool_Process"));
}

TEST_CASE("encoder engine: a Stereo Tool too slow is retried automatically, a licence bypass is not") {
  UseFake use;
  StDir dir("retry");
  struct Restore {
    ~Restore() {
      be::detail::testing::set_processing_retry_ms({}, 0);
      be::detail::testing::set_processing_stop_timeout_ms(0);
    }
  } restore;
  be::detail::testing::set_processing_retry_ms({400, 800}, 60000);
  be::detail::testing::set_processing_stop_timeout_ms(1000);
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Wolna");
  st.processing = processing("Wolny.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("Wolny.sts", fake_preset(0.5, 128)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  const std::string who = "Processing stacji „Radio Wolna”: ";
  auto too_slow = [&] { return processing_of(engine, "s1").reason_code == be::ProcessingReason::TooSlow; };

  fake().set("process_sleep_us", 300000);  // stuck for 300 ms in every call
  REQUIRE(wait_for(too_slow, 5000));
  const auto waiting = processing_of(engine, "s1");
  CHECK(waiting.state == be::ProcessingState::Bypass);
  CHECK(wait_for([&] { return logged(who + "automatyczna ponowna próba za 400 ms"); }, 2000));
  // still too slow at the first retry: it fails, the next one waits longer
  REQUIRE(wait_for([&] { return logged(who + "automatyczna ponowna próba nr 1"); }, 3000));
  REQUIRE(wait_for([&] { return logged(who + "ponowna próba nieudana: Stereo Tool znów nie nadąża"); }, 5000));
  CHECK(wait_for([&] { return logged(who + "automatyczna ponowna próba za 800 ms"); }, 2000));
  CHECK(processing_of(engine, "s1").retries == 1);
  fake().set("process_sleep_us", 0);  // fast again: the next retry succeeds
  REQUIRE(wait_for([&] { return logged(who + "automatyczna ponowna próba nr 2"); }, 5000));
  REQUIRE(processing_running(engine, "s1"));
  CHECK(wait_for([&] { return logged(who + "ponowna próba udana: Stereo Tool znów przetwarza sygnał"); }, 2000));
  CHECK(processing_of(engine, "s1").retry_in_s == -1);
  CHECK(fake().value("created") == 1);  // the instance was kept: a restart, not a new one

  // a licence that does not cover the settings: no automatic retry
  const size_t retries_logged = log_lines_with(who + "automatyczna ponowna próba");
  fake().set("license_valid", 0);
  REQUIRE(wait_for([&] { return processing_of(engine, "s1").reason_code == be::ProcessingReason::Licence; }, 5000));
  bp::sleep_ns(1'500'000'000);
  const auto licence = processing_of(engine, "s1");
  CHECK(licence.reason_code == be::ProcessingReason::Licence);
  CHECK(licence.retry_in_s == -1);
  CHECK(licence.unlicensed_features == "Fake: Declipper, Fake: Multiband");
  CHECK(log_lines_with(who + "automatyczna ponowna próba") == retries_logged);
  CHECK(logged(who + "licencja Stereo Tool nie obejmuje funkcji"));
  // the worker's own line names the station like the engine's, with the features
  CHECK(logged(who + "licencja Stereo Tool nie obejmuje funkcji: Fake: Declipper, Fake: Multiband"));
  CHECK(log_lines_with("Processing stacji s1:") == 0);
  CHECK(log_lines_with(kKey1) == 0);
}

TEST_CASE("encoder engine: a preset uploaded after the station asked for it starts its processing") {
  UseFake use;
  StDir dir("late");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Spóźniona");
  st.processing = processing("Late.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(wait_for([&] { return processing_of(engine, "s1").state == be::ProcessingState::Error; }, 5000));
  CHECK(processing_of(engine, "s1").reason_code == be::ProcessingReason::Setup);
  REQUIRE(engine.put_preset("late.STS", fake_preset(0.5, 64)).ok());  // any case
  CHECK(processing_running(engine, "s1", 5000));
  CHECK(fake().text("load_path") == std::optional<std::string>((fs::path(dir.presets()) / "late.STS").string()));
}

TEST_CASE("encoder engine: at the end of the service Stereo Tool instances are not deleted, a removed station's is (ruling S7)") {
  UseFake use;
  StDir dir("exit");
  struct DropLive {
    ~DropLive() { fake().drop_live(); }  // the end of the process, for the tests that follow
  } drop;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  for (int i = 1; i <= 3; ++i) {
    be::StationConfig st = station("s" + std::to_string(i), "Radio Koniec " + std::to_string(i));
    st.processing = processing("P.sts", i == 1 ? kKey1 : kKey2);
    cfg.stations.push_back(st);
  }
  // and one whose preset is not there: its worker runs without an instance (nothing to leave)
  be::StationConfig no_preset = station("s4", "Radio Bez Presetu");
  no_preset.processing = processing("Brak.sts", kKey2);
  cfg.stations.push_back(no_preset);
  cfg = ready(cfg);
  be::EncoderEngineOptions opts = st_options(dir);
  opts.keep_stereotool_at_stop = true;
  be::EncoderEngine engine(opts);
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  for (const char* id : {"s1", "s2", "s3"}) REQUIRE(processing_running(engine, id));
  REQUIRE(wait_for([&] { return processing_of(engine, "s4").state == be::ProcessingState::Error; }, 5000));
  REQUIRE(wait_for([] { return fake().value("live") == 3; }, 5000));
  const long long deleted0 = fake().value("delete_calls");  // s4's instance, deleted when its preset failed
  fake().set("delete_sleep_us", 1'000'000);  // like the real library: deleting takes a while
  // a station removed while the service runs: its instance is deleted
  be::EncoderConfig fewer = cfg;
  fewer.stations.erase(fewer.stations.begin() + 2);
  engine.apply_config(fewer);
  CHECK(wait_for([&] { return fake().value("deleted") == deleted0 + 1; }, 5000));
  CHECK(fake().value("live") == 2);
  // the end of the service: no Delete, and no waiting for one
  const int64_t t0 = bp::monotonic_ns();
  engine.stop();
  const int64_t took = bp::monotonic_ns() - t0;
  CHECK_MESSAGE(took < 900'000'000, "stop took " << took / 1'000'000 << " ms");
  CHECK(fake().value("delete_calls") == deleted0 + 1);
  CHECK(fake().value("live") == 2);
  CHECK(logged("instancje Stereo Tool (2) nie są usuwane przy zatrzymaniu usługi"));
}

TEST_CASE("encoder engine: the status does not wait for the Stereo Tool library to load") {
  UseFake use;
  StDir dir("loading");
  REQUIRE(fake().value("version_calls") == 0);
  fake().set("version_sleep_us", 1'000'000);  // the library takes a second to load
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Ładowanie");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);  // the worker's Stereo Tool thread loads it
  REQUIRE(wait_for([] { return fake().value("version_calls") == 1; }, 2000));
  const int64_t t0 = bp::monotonic_ns();
  const be::EncoderStatus s = engine.status();
  const int64_t took = bp::monotonic_ns() - t0;
  CHECK_MESSAGE(took < 200'000'000, "status took " << took / 1'000'000 << " ms");
  CHECK(s.stereotool.loading);
  CHECK_FALSE(s.stereotool.available);
  CHECK(processing_running(engine, "s1", 5000));
  CHECK_FALSE(engine.status().stereotool.loading);
  CHECK(engine.status().stereotool.available);
}

TEST_CASE("encoder engine: the retry waits repeat their last value and start over after a good stretch") {
  UseFake use;
  StDir dir("retry2");
  struct Restore {
    ~Restore() {
      be::detail::testing::set_processing_retry_ms({}, 0);
      be::detail::testing::set_processing_stop_timeout_ms(0);
    }
  } restore;
  be::detail::testing::set_processing_retry_ms({300, 500}, 1500);  // stands for 30 s, 2 min ... and 10 min
  be::detail::testing::set_processing_stop_timeout_ms(1000);
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Licznik");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  const std::string who = "Processing stacji „Radio Licznik”: ";
  auto lines = [&](const std::string& what) { return log_lines_with(who + what); };

  fake().set("process_sleep_us", 300000);  // stuck: every retry fails
  REQUIRE(wait_for([&] { return lines("automatyczna ponowna próba nr 3") == 1; }, 10000));
  CHECK(lines("automatyczna ponowna próba za 300 ms") == 1);
  CHECK(wait_for([&] { return lines("automatyczna ponowna próba za 500 ms") == 2; }, 3000));  // the last wait repeats
  fake().set("process_sleep_us", 0);
  REQUIRE(processing_running(engine, "s1", 5000));
  CHECK(processing_of(engine, "s1").retries >= 3);
  // a good stretch longer than the reset: the count starts over
  CHECK(wait_for([&] { return processing_of(engine, "s1").retries == 0; }, 4000));
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Running);
  fake().set("process_sleep_us", 300000);
  CHECK(wait_for([&] { return lines("automatyczna ponowna próba za 300 ms") == 2; }, 5000));  // the first wait again
  fake().set("process_sleep_us", 0);
}

TEST_CASE("encoder engine: a slow save bypasses as a save, never as too slow, and needs no retry") {
  UseFake use;
  StDir dir("slowsave");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Wolny Zapis");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  fake().set("save_sleep_us", 600000);
  const be::PresetResult r = engine.save_processing_preset("s1", "Wolny.sts");
  fake().set("save_sleep_us", 0);
  CHECK_MESSAGE(r.ok(), r.message);
  CHECK(processing_running(engine, "s1", 3000));
  bp::sleep_ns(300'000'000);
  const std::string who = "Processing stacji „Radio Wolny Zapis”: ";
  CHECK(logged(who + "zapis presetu"));
  CHECK(log_lines_with(who + "Stereo Tool nie nadąża") == 0);
  CHECK(log_lines_with(who + "automatyczna ponowna próba") == 0);
  CHECK(fake().value("created") == 1);
}

TEST_CASE("encoder engine: a station added as another is deleted gets a new id and a new instance (ruling S4)") {
  UseFake use;
  StDir dir("reuse");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("", "Radio Stara");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  REQUIRE(cfg.stations[0].id == "s1");
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));

  // one update: s1 goes, a new station comes (the counter travels with the configuration)
  be::StationConfig added = station("", "Radio Nowa");
  added.processing = processing("P.sts", kKey2);
  cfg.stations = {added};
  cfg = ready(cfg);
  CHECK(cfg.stations[0].id == "s2");
  engine.apply_config(cfg);
  REQUIRE(processing_running(engine, "s2"));
  CHECK(fake().value("created") == 2);  // its own instance, not s1's with its settings
  CHECK(fake().text("create_name") == std::optional<std::string>("station-s2"));
  CHECK(fake().text("create_key") == std::optional<std::string>(kKey2));
  CHECK(wait_for([] { return fake().value("live") == 1; }, 5000));
}

TEST_CASE("encoder engine: a web interface port is bound by one instance at a time") {
  UseFake use;
  StDir dir("port");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stereotool_web_port_base = 8500;
  be::StationConfig st = station("s1", "Radio Port");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  SlowBuild slow(150'000'000);  // the old instance would still hold the port while the new one starts

  SUBCASE("a new key: the new instance takes the port once the old one is gone") {
    cfg.stations[0].processing.license_key = kKey2;
    engine.apply_config(ready(cfg));
    REQUIRE(processing_running(engine, "s1"));
    CHECK(fake().value("created") == 2);
    CHECK(processing_of(engine, "s1").web_port == 8500);
  }
  SUBCASE("a slot freed and given to a new station in the same update") {
    be::StationConfig added = station("", "Radio Nowa");
    added.processing = processing("P.sts", kKey2);
    cfg.stations = {added};
    cfg = ready(cfg);
    REQUIRE(cfg.stations[0].processing.web_port_slot == 0);
    engine.apply_config(cfg);
    REQUIRE(processing_running(engine, cfg.stations[0].id));
    CHECK(processing_of(engine, cfg.stations[0].id).web_port == 8500);
  }
  CHECK(fake().value("port_conflicts") == 0);
  CHECK(fake().value("live") == 1);
}

TEST_CASE("encoder engine: a new instance without a web port waits for the old one to let go of the station's home folder") {
  UseFake use;
  StDir dir("samehome");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {station("s1", "Radio Ten Sam Folder")};
  cfg.stations[0].processing = processing("P.sts");
  REQUIRE(cfg.stations[0].processing.web_port_slot == -1);  // not normalized: no web port (port 0)
  REQUIRE(be::validate_config(cfg).empty());
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  fake().set("delete_sleep_us", 400'000);  // the old instance lets go of the folder slowly
  cfg.stations[0].processing.license_key = kKey2;  // a new instance, in the same station-s1 folder
  engine.apply_config(cfg);
  REQUIRE(processing_running(engine, "s1"));
  CHECK(fake().value("created") == 2);
  CHECK(fake().value("home_conflicts") == 0);
  CHECK(fake().value("live") == 1);
}

TEST_CASE("encoder engine: one preset save at a time in the whole service") {
  UseFake use;
  StDir dir("onesave");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {station("s1", "Radio Zapis Jeden"), station("s2", "Radio Zapis Dwa")};
  cfg.stations[0].processing = processing("P.sts");
  cfg.stations[1].processing = processing("P.sts", kKey2);
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  REQUIRE(processing_running(engine, "s2"));
  fake().set("save_sleep_us", 800'000);
  be::PresetResult first;
  std::thread a([&] { first = engine.save_processing_preset("s1", "Pierwszy.sts"); });
  bp::sleep_ns(200'000'000);
  const int64_t t0 = bp::monotonic_ns();
  const be::PresetResult second = engine.save_processing_preset("s2", "Drugi.sts");  // another station
  const int64_t took = bp::monotonic_ns() - t0;
  a.join();
  CHECK(first.ok());
  CHECK(second.error == be::PresetError::Busy);
  CHECK_MESSAGE(second.message.find("już trwa") != std::string::npos, second.message);
  CHECK_MESSAGE(took < 300'000'000, "the refusal waited " << took / 1'000'000 << " ms");  // no HTTP thread held
  std::string data;
  CHECK_FALSE(engine.get_preset("Drugi.sts", &data).ok());
  fake().set("save_sleep_us", 0);
  CHECK(engine.save_processing_preset("s2", "Drugi.sts").ok());  // once the first is over
}

TEST_CASE("encoder engine: a configuration without a web port slot runs Stereo Tool without its web interface") {
  UseFake use;
  StDir dir("noslot");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {station("s1", "Radio Bez Portu"), station("s2", "Radio Bez Slotu")};
  cfg.stations[1].processing = processing("P.sts");
  REQUIRE(cfg.stations[1].processing.web_port_slot == -1);  // not normalized
  REQUIRE(be::validate_config(cfg).empty());
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s2"));
  CHECK(processing_of(engine, "s2").web_port == 0);  // never a port taken from the position
  CHECK(fake().sts("station-s2", be::detail::st::kParamWebserverEnable) == std::optional<std::string>("0"));
}

namespace {

// The crash-loop guard's marker of a test engine (EncoderEngineOptions::crash_marker_path).
struct Marker {
  std::string path;
  explicit Marker(const StDir& dir) : path((fs::path(dir.path) / "running.marker").string()) { fs::create_directories(dir.path); }
  bool exists() const { return fs::exists(path); }
  std::string text() const { return read_file(path); }
  void write(const std::string& text) const { write_file(path, text); }
};

be::EncoderConfig guarded_config() {
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig a = station("s1", "Radio Awaria");
  a.processing = processing("P.sts");
  be::StationConfig b = station("s2", "Radio Awaria Dwa");
  b.processing = processing("P.sts", kKey2);
  cfg.stations = {a, b, station("s3", "Radio Bez Processingu")};
  return ready(cfg);
}

bool held(const be::EncoderEngine& e, const std::string& id) {
  const be::ProcessingStatus p = processing_of(e, id);
  return p.state == be::ProcessingState::Error && p.reason_code == be::ProcessingReason::PreviousCrash &&
         p.reason.find("awaria przy poprzednim uruchomieniu") != std::string::npos;
}

}  // namespace

TEST_CASE("encoder engine: the crash-loop guard's marker: written at start, says when processing ran, removed by a clean stop") {
  UseFake use;
  StDir dir("marker");
  const Marker marker(dir);
  be::EncoderEngineOptions opts = st_options(dir);
  opts.crash_marker_path = marker.path;
  {
    be::EncoderConfig cfg = guarded_config();
    cfg.stations[0].processing.enabled = false;
    cfg.stations[1].processing.enabled = false;
    be::EncoderEngine engine(opts);
    REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
    std::string err;
    REQUIRE_MESSAGE(engine.start(cfg, &err), err);
    CHECK(marker.text() == "processing=0\n");  // nothing of Stereo Tool ran yet
    cfg.stations[0].processing.enabled = true;
    engine.apply_config(cfg);
    CHECK(marker.text() == "processing=1\n");  // from the first worker on
    REQUIRE(processing_running(engine, "s1"));
    engine.stop();
    CHECK_FALSE(marker.exists());  // a clean stop
  }
  {
    // an unclean end of a run without processing: no guard
    marker.write("processing=0\n");
    be::EncoderEngine engine(opts);
    std::string err;
    REQUIRE_MESSAGE(engine.start(guarded_config(), &err), err);
    CHECK(processing_running(engine, "s1"));
    CHECK(processing_running(engine, "s2"));
    CHECK(marker.text() == "processing=1\n");
  }
  CHECK_FALSE(marker.exists());  // the destructor stops cleanly too
}

TEST_CASE("encoder engine: the crash-loop guard's marker goes when the stop begins, not after a slow end") {
  UseFake use;
  StDir dir("marker-slow");
  const Marker marker(dir);
  be::EncoderEngineOptions opts = st_options(dir);
  opts.crash_marker_path = marker.path;
  be::EncoderEngine engine(opts);
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(guarded_config(), &err), err);
  REQUIRE(processing_running(engine, "s1"));
  REQUIRE(marker.text() == "processing=1\n");
  fake().set("delete_sleep_us", 1'500'000);  // the end of the stop takes a while
  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    engine.stop();
    stopped = true;
  });
  // Windows may end the service before a slow stop is over (a reboot): the marker is gone by then
  CHECK(wait_for([&] { return !marker.exists(); }, 1000));
  CHECK_FALSE(stopped.load());
  stopper.join();
  CHECK_FALSE(marker.exists());
}

TEST_CASE("encoder engine: after an unclean end with processing, every processing station starts without it until restarted by hand") {
  UseFake use;
  StDir dir("guard");
  const Marker marker(dir);
  marker.write("processing=1\n");  // the previous run crashed with Stereo Tool running
  be::EncoderEngineOptions opts = st_options(dir);
  opts.crash_marker_path = marker.path;
  be::EncoderEngine engine(opts);
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  REQUIRE(engine.put_preset("Q.sts", fake_preset(0.25, 32)).ok());
  be::EncoderConfig cfg = guarded_config();
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  CHECK(held(engine, "s1"));
  CHECK(held(engine, "s2"));
  CHECK(processing_of(engine, "s3").state == be::ProcessingState::Off);
  CHECK(logged("poprzednie uruchomienie usługi nie zakończyło się poprawnie"));
  CHECK(marker.text() == "processing=0\n");
  bp::sleep_ns(300'000'000);
  CHECK(fake().value("create_calls") == 0);  // Stereo Tool is not touched at all
  for (const auto& st : engine.status().stations) CHECK(st.running);  // the stations play, unprocessed

  // "Uruchom ponownie processing" of one station: it runs, the other stays held
  REQUIRE_MESSAGE(engine.restart_processing("s1", &err), err);
  CHECK(processing_running(engine, "s1"));
  CHECK(marker.text() == "processing=1\n");
  CHECK(held(engine, "s2"));
  CHECK(logged("Processing stacji „Radio Awaria”: ponowne uruchomienie (ręczne)"));
  // a save of the other station in the panel
  CHECK(engine.release_processing_hold("s2"));
  CHECK(processing_running(engine, "s2"));
  CHECK_FALSE(engine.release_processing_hold("s2"));  // not held any more
  CHECK(fake().value("created") == 2);
  engine.stop();
  CHECK_FALSE(marker.exists());
}

TEST_CASE("encoder engine: a held station gets its processing back when its processing settings change") {
  UseFake use;
  StDir dir("guardchange");
  const Marker marker(dir);
  marker.write("processing=1\n");
  be::EncoderEngineOptions opts = st_options(dir);
  opts.crash_marker_path = marker.path;
  be::EncoderEngine engine(opts);
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  REQUIRE(engine.put_preset("Q.sts", fake_preset(0.25, 32)).ok());
  be::EncoderConfig cfg = guarded_config();
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(held(engine, "s1"));
  // a change elsewhere (a name) keeps it held
  cfg.stations[2].name = "Radio Inna Nazwa";
  engine.apply_config(cfg);
  CHECK(held(engine, "s1"));
  // another preset for s1: back
  cfg.stations[0].processing.preset = "Q.sts";
  engine.apply_config(cfg);
  CHECK(processing_running(engine, "s1"));
  CHECK(held(engine, "s2"));
  // switched off and on again: back as well
  cfg.stations[1].processing.enabled = false;
  engine.apply_config(cfg);
  CHECK(processing_of(engine, "s2").state == be::ProcessingState::Off);
  cfg.stations[1].processing.enabled = true;
  engine.apply_config(cfg);
  CHECK(processing_running(engine, "s2"));
}

TEST_CASE("encoder engine: a save Stereo Tool does not finish in time is reported, processing comes back after it") {
  UseFake use;
  StDir dir("hungsave");
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Zawieszony Zapis");
  st.processing = processing("P.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(st_options(dir));
  REQUIRE(engine.put_preset("P.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(processing_running(engine, "s1"));
  be::detail::testing::set_save_preset_timeout_ms(400);
  fake().set("save_sleep_us", 1'500'000);
  const be::PresetResult r = engine.save_processing_preset("s1", "Zawieszony.sts");
  be::detail::testing::set_save_preset_timeout_ms(0);
  CHECK(r.error == be::PresetError::Failed);
  CHECK_MESSAGE(r.message.find("nie zakończył zapisu presetu w ciągu 0,4 s") != std::string::npos, r.message);
  const std::string who = "Processing stacji „Radio Zawieszony Zapis”: ";
  CHECK(logged(who + "nie zapisano presetu „Zawieszony.sts”: Stereo Tool nie zakończył zapisu presetu w ciągu 0,4 s"));
  // still saving: the status says so, the reason stays "saving"
  const be::ProcessingStatus during = processing_of(engine, "s1");
  CHECK(during.state == be::ProcessingState::Bypass);
  CHECK(during.reason_code == be::ProcessingReason::Saving);
  CHECK_MESSAGE(during.reason.find("nie zakończył zapisu") != std::string::npos, during.reason);
  fake().set("save_sleep_us", 0);
  CHECK(processing_running(engine, "s1", 5000));
  CHECK(log_lines_with(who + "automatyczna ponowna próba") == 0);
  CHECK(engine.list_presets().size() == 1);  // the late file is not a preset
}

// ---- the AES67 output of a station (1.2), received on 127.0.0.1 ----

namespace {

be::Aes67Status aes67_of(const be::EncoderEngine& e, const std::string& id) {
  for (const auto& st : e.status().stations)
    if (st.id == id) return st.aes67;
  FAIL("no station " << id);
  return {};
}

// Tests start sending at once (no PTP master on loopback) and announce nothing.
be::EncoderEngineOptions aes67_options(be::EncoderEngineOptions o) {
  o.aes67_require_ptp = false;
  o.enable_sap = false;
  return o;
}

// One input (20 ms receive latency: 960 frames) and the AES67 output switched on.
be::StationConfig aes67_station(const std::string& group, uint16_t port) {
  be::StationConfig st = station("s1", "Radio AES67");
  be::InputConfig a;
  a.sdp = make_sdp("Wejście", group, port);
  a.latency_us = 20000;
  st.inputs = {a};
  st.aes67_output.enabled = true;
  return st;
}

int32_t l24(int32_t v) { return v & int32_t(0xffffff00); }  // what an L24 packet keeps

void wait_media(const be::EncoderEngine& e, uint64_t frames) {
  const uint64_t from = media_now(e);
  REQUIRE(wait_for([&] { return media_now(e) > from + frames; }, int(frames / 48) + 10000));
}

}  // namespace

TEST_CASE("encoder engine: a station's AES67 output carries its mix bit for bit at a constant delay") {
  // one input at 0 dB carrying sig_a, no processing; the input ring and the mix share the media
  // timeline (mix position p = input sample p), and packet T carries mix position T - D
  const std::string group = "239.69.160.1";
  constexpr uint16_t kInPort = 15260;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {aes67_station(group, kInPort)};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(no_ptp()));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Sending; }, 5000));
  const be::Aes67Status first = aes67_of(engine, "s1");
  CHECK(first.address == be::aes67_default_address(cfg.stations[0].processing.web_port_slot));
  CHECK(first.name == "Radio AES67");
  CHECK(first.ptime_us == 1000);
  CHECK(first.reason.empty());
  AesReceiver out(engine, first.address);
  REQUIRE(wait_for([&] { return out.packets() > 2000; }, 10000));
  const be::Aes67Status s = aes67_of(engine, "s1");
  CHECK(s.gaps == 0);
  CHECK(s.send_errors == 0);
  CHECK(s.packets >= out.packets());
  constexpr uint64_t kD = 960 + 480 + 0 + 480;  // latency + tick + no processing + margin
  CHECK(s.output_latency_ms == doctest::Approx(kD / 48.0));
  const uint64_t end = media_now(engine) - 4800;
  for (uint64_t t = end - 1000; t < end; ++t) {
    REQUIRE(out.at(t, 0) == sig_a(t - kD, 0));
    REQUIRE(out.at(t, 1) == sig_a(t - kD, 1));
  }
}

TEST_CASE("encoder engine: the AES67 output follows Stereo Tool") {
  UseFake use;
  StDir dir("aes67_st");
  const std::string group = "239.69.161.1";
  constexpr uint16_t kInPort = 15261;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = aes67_station(group, kInPort);
  st.processing = processing("Half.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(st_options(dir)));
  REQUIRE(engine.put_preset("Half.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(processing_running(engine, "s1"));
  const uint64_t pd = uint64_t(std::lround(processing_of(engine, "s1").latency_ms * 48.0));
  CHECK(pd == 64 + 240);  // Stereo Tool's latency + the crossfade margin
  const uint64_t kD = 960 + 480 + pd + 480;
  // the housekeeping moves D once the worker's delay grew (it starts unprocessed, undelayed)
  REQUIRE(wait_for([&] { return std::abs(aes67_of(engine, "s1").output_latency_ms - kD / 48.0) < 1e-6; }, 2000));
  CHECK(aes67_of(engine, "s1").output_latency_ms == doctest::Approx(kD / 48.0));
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  REQUIRE(wait_for([&] { return out.packets() > 1000; }, 10000));
  CHECK(aes67_of(engine, "s1").state == be::Aes67State::Sending);
  const uint64_t end = media_now(engine) - 4800;
  for (uint64_t t = end - 1000; t < end; ++t) {
    REQUIRE(out.at(t, 0) == l24(sig_a(t - kD, 0) / 2));  // the fake: x 0.5, exact in float
    REQUIRE(out.at(t, 1) == l24(sig_a(t - kD, 1) / 2));
  }
}

TEST_CASE("encoder engine: switching processing on and off never repeats audio on the AES67 output") {
  UseFake use;
  StDir dir("aes67_switch");
  const std::string group = "239.69.162.1";
  constexpr uint16_t kInPort = 15262;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = aes67_station(group, kInPort);
  st.processing = processing("Unity.sts");
  st.processing.enabled = false;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(st_options(dir)));
  // a latency longer than what the worker has written ahead of the sending (as the real ~90 ms)
  REQUIRE(engine.put_preset("Unity.sts", fake_preset(1.0, 2048)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Sending; }, 5000));
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  REQUIRE(wait_for([&] { return out.packets() > 500; }, 10000));
  const uint64_t gaps_before = aes67_of(engine, "s1").gaps;

  out.start_recording(media_now(engine));
  // like the real library, Stereo Tool is ready only after the worker started unprocessed: its
  // delay then grows while it runs
  fake().set("create_sleep_us", 300000);
  cfg.stations[0].processing.enabled = true;
  engine.apply_config(ready(cfg));
  REQUIRE(processing_running(engine, "s1"));
  const uint64_t pd = uint64_t(std::lround(processing_of(engine, "s1").latency_ms * 48.0));
  wait_media(engine, 48000);
  const uint64_t off = media_now(engine);
  const uint64_t gaps_on = aes67_of(engine, "s1").gaps;
  cfg.stations[0].processing.enabled = false;
  engine.apply_config(ready(cfg));
  wait_media(engine, 3 * 48000 + 4800);
  uint64_t from = 0;
  const auto rec = out.stop_recording(&from);
  REQUIRE(rec.size() > 3 * 48000);
  REQUIRE(off > from);
  REQUIRE(off - from < rec.size());

  // both switches re-map the output with a fade: no gap, and the output pauses only about as
  // long as the delay changes (the processing delay, plus the ramps and a station tick)
  MESSAGE("gaps after on: " << gaps_on - gaps_before);
  CHECK(aes67_of(engine, "s1").gaps == gaps_before);
  struct Silence {
    size_t at = 0, len = 0;
  };
  auto longest_silence = [&](size_t a, size_t b) {
    Silence longest, run;
    for (size_t i = a; i < b; ++i) {
      if (rec[i][0] != 0 || rec[i][1] != 0) {
        run = Silence{};
        continue;
      }
      if (run.len++ == 0) run.at = i;
      if (run.len > longest.len) longest = run;
    }
    return longest;
  };
  auto loudest = [&](size_t a, size_t b) {
    int64_t m = 0;
    for (size_t i = a; i < b; ++i) m = std::max({m, std::llabs(rec[i][0]), std::llabs(rec[i][1])});
    return m;
  };
  const Silence on = longest_silence(0, size_t(off - from));
  const Silence quiet = longest_silence(size_t(off - from), rec.size());
  MESSAGE("longest silence: switch on " << on.len << ", switch off " << quiet.len << " frames (processing delay " << pd << ")");
  CHECK(on.len <= pd + 720);
  CHECK(quiet.len <= pd + 720);
  // no hard cut: the 24 frames on either side of each pause are within a 5 ms ramp of the ±0.5 FS signal
  constexpr int64_t kRampEdge = (int64_t(1) << 30) * 24 / 240;
  for (const Silence& z : {on, quiet}) {
    REQUIRE(z.at >= 24);
    REQUIRE(z.at + z.len + 24 <= rec.size());
    CHECK(loudest(z.at - 24, z.at) <= kRampEdge);
    CHECK(loudest(z.at + z.len, z.at + z.len + 24) <= kRampEdge);
  }

  // source positions by their (left, right) pair over the range the output can carry
  constexpr uint64_t kAmbiguous = UINT64_MAX;
  auto key = [](int32_t l, int32_t r) { return uint64_t(uint32_t(l)) << 32 | uint32_t(r); };
  std::unordered_map<uint64_t, uint64_t> where;
  where.reserve(rec.size() + 48000);
  for (uint64_t p = from - 48000; p < from + rec.size(); ++p) {
    auto [it, fresh] = where.emplace(key(sig_a(p, 0), sig_a(p, 1)), p);
    if (!fresh) it->second = kAmbiguous;
  }
  // silence and ramp frames match no pair: skipped
  std::unordered_set<uint64_t> seen;
  size_t matched = 0, repeated = 0, backwards = 0;
  uint64_t last = 0;
  for (const auto& f : rec) {
    const auto it = where.find(key(f[0], f[1]));
    if (it == where.end() || it->second == kAmbiguous) continue;
    const uint64_t src = it->second;
    if (!seen.insert(src).second) ++repeated;
    if (matched > 0 && src <= last) ++backwards;
    last = src;
    ++matched;
  }
  CHECK(matched > rec.size() * 9 / 10);
  CHECK(repeated == 0);
  CHECK(backwards == 0);
}

TEST_CASE("encoder engine: without PTP lock the AES67 output does not send") {
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.ptp_domain = 89;  // no master on loopback
  be::StationConfig st = station("s1", "Radio Bez Zegara");
  st.aes67_output.enabled = true;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngineOptions opts;  // PTP on, aes67_require_ptp by default
  opts.enable_sap = false;
  be::EncoderEngine engine(opts);
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  bp::sleep_ns(1'000'000'000);
  CHECK(out.packets() == 0);
  const be::Aes67Status s = aes67_of(engine, "s1");
  CHECK(s.state == be::Aes67State::WaitingForPtp);
  CHECK_MESSAGE(s.reason.find("czeka na zegar PTP") != std::string::npos, s.reason);
  CHECK(s.packets == 0);
}

TEST_CASE("encoder engine: with PTP switched off the AES67 output says it needs the PTP clock") {
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Bez PTP");
  st.aes67_output.enabled = true;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngineOptions opts = no_ptp();  // aes67_require_ptp by default
  opts.enable_sap = false;
  be::EncoderEngine engine(opts);
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  bp::sleep_ns(500'000'000);
  CHECK(out.packets() == 0);
  const be::Aes67Status s = aes67_of(engine, "s1");
  CHECK(s.state == be::Aes67State::WaitingForPtp);
  CHECK(s.reason == "PTP wyłączony — wyjście AES67 wymaga zegara PTP");
}

TEST_CASE("encoder engine: after a lock the AES67 output keeps sending in holdover") {
  constexpr int kDomain = 87;
  LoopbackPtpMaster master(kDomain);
  std::string err;
  REQUIRE_MESSAGE(master.engine->start(&err), err);
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.ptp_domain = kDomain;
  be::StationConfig st = station("s1", "Radio Holdover");
  st.aes67_output.enabled = true;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngineOptions opts;  // PTP on, aes67_require_ptp by default
  opts.enable_sap = false;
  be::EncoderEngine engine(opts);
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(wait_for([&] { return engine.status().ptp.state == bam::PtpState::Locked; }, 40000));
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Sending; }, 2000));
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  REQUIRE(wait_for([&] { return out.packets() > 200; }, 5000));

  master.engine->stop();
  uint64_t last = out.packets();
  for (int i = 0; i < 4; ++i) {  // 2 s
    bp::sleep_ns(500'000'000);
    const uint64_t now = out.packets();
    CHECK(now > last);
    last = now;
  }
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Holdover; }, 10000));
  const be::Aes67Status s = aes67_of(engine, "s1");
  CHECK(s.reason == "zegar podtrzymany (brak mastera PTP)");
  const uint64_t in_holdover = out.packets();
  bp::sleep_ns(500'000'000);
  CHECK(out.packets() > in_holdover);
  CHECK(log_lines_with("Wyjście AES67: zegar podtrzymany") == 1);
}

TEST_CASE("encoder engine: a stalled Stereo Tool gives silence on the AES67 output and counts gaps, then the bypass refills it") {
  UseFake use;
  StDir dir("aes67_stall");
  const std::string group = "239.69.164.1";
  constexpr uint16_t kInPort = 15264;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = aes67_station(group, kInPort);
  st.processing = processing("Half.sts");
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(st_options(dir)));
  REQUIRE(engine.put_preset("Half.sts", fake_preset(0.5, 64)).ok());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(processing_running(engine, "s1"));
  constexpr double kDms = (960 + 480 + 64 + 240 + 480) / 48.0;
  REQUIRE(wait_for([&] { return std::abs(aes67_of(engine, "s1").output_latency_ms - kDms) < 1e-6; }, 2000));
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  REQUIRE(wait_for([&] { return out.packets() > 500; }, 10000));
  const be::Aes67Status before = aes67_of(engine, "s1");

  fake().set("process_sleep_us", 300000);  // stuck for 300 ms in every call
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").gaps > before.gaps; }, 3000));
  const uint64_t received = out.packets();
  CHECK(aes67_of(engine, "s1").packets > before.packets);
  // the worker bypasses after 200 ms behind: the content is back and the gaps stop
  bool stopped = false;
  uint64_t gaps = aes67_of(engine, "s1").gaps;
  int64_t since = bp::monotonic_ns();
  const int64_t deadline = since + 2'000'000'000;
  while (!stopped && bp::monotonic_ns() < deadline + 500'000'000) {
    bp::sleep_ns(50'000'000);
    const uint64_t g = aes67_of(engine, "s1").gaps;
    if (g != gaps) {
      gaps = g;
      since = bp::monotonic_ns();
      if (since > deadline) break;
    } else if (bp::monotonic_ns() - since >= 500'000'000) {
      stopped = true;
    }
  }
  CHECK_MESSAGE(stopped, "gaps still rising: " << gaps);
  CHECK(out.packets() > received);
  CHECK(processing_of(engine, "s1").state == be::ProcessingState::Bypass);
  fake().set("process_sleep_us", 0);
}

TEST_CASE("encoder engine: an AES67 multicast interface that failed is tried again") {
  // like a card that is not up yet when the service starts (an explicit interface_ip at boot)
  struct Seams {
    Seams() {
      be::detail::testing::fail_next_aes67_interfaces(1);
      be::detail::testing::set_input_retry_ms(300);
    }
    ~Seams() {
      be::detail::testing::fail_next_aes67_interfaces(0);
      be::detail::testing::set_input_retry_ms(0);
    }
  } seams;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio Późna Karta");
  st.aes67_output.enabled = true;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(no_ptp()));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  const be::Aes67Status failed = aes67_of(engine, "s1");
  CHECK(failed.state == be::Aes67State::Error);
  CHECK(failed.reason == "nie można nadawać multicastu przez kartę 127.0.0.1");
  AesReceiver out(engine, failed.address);
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Sending; }, 3000));
  CHECK(aes67_of(engine, "s1").reason.empty());
  CHECK(wait_for([&] { return out.packets() > 100; }, 5000));
  CHECK(log_lines_with("Wyjście AES67: nadawanie przez kartę 127.0.0.1 działa") == 1);
}

TEST_CASE("encoder engine: disabling the AES67 output stops its packets; a station without one sends none") {
  const std::string group = "239.69.165.1";
  constexpr uint16_t kInPort = 15265;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {aes67_station(group, kInPort), station("s2", "Radio Bez Wyjścia")};
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(no_ptp()));
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::Sending; }, 5000));
  const be::Aes67Status quiet = aes67_of(engine, "s2");
  CHECK(quiet.state == be::Aes67State::Off);
  REQUIRE(quiet.address != aes67_of(engine, "s1").address);
  AesReceiver out(engine, aes67_of(engine, "s1").address);
  AesReceiver none(engine, quiet.address);
  REQUIRE(wait_for([&] { return out.packets() > 500; }, 10000));
  CHECK(none.packets() == 0);
  CHECK(aes67_of(engine, "s2").packets == 0);

  cfg.stations[0].aes67_output.enabled = false;
  engine.apply_config(ready(cfg));
  const be::Aes67Status s = aes67_of(engine, "s1");
  CHECK(s.state == be::Aes67State::Off);
  CHECK(s.packets == 0);
  bp::sleep_ns(100'000'000);
  const uint64_t p = out.packets();
  bp::sleep_ns(1'000'000'000);
  CHECK(out.packets() == p);
  CHECK(none.packets() == 0);
}

#endif  // BAM_FAKE_STEREOTOOL_PATH

#endif  // BAM_WITH_CODECS

// ---- SAP announcements of the AES67 output, on 127.0.0.1 ----

namespace {

// Listens on the SAP group (loopback): every packet received so far.
struct SapListener {
  std::optional<bp::UdpSocket> sock;
  std::vector<bam::SapPacket> got;
  SapListener() {
    std::string err;
    sock = bp::UdpSocket::open(bam::kSapPort, &err);
    REQUIRE_MESSAGE(sock, err);
    REQUIRE_MESSAGE(sock->join(bam::kSapGroup, kLoopback, "", &err), err);
  }
  void poll() {
    std::vector<uint8_t> buf(4096);
    int n;
    while ((n = sock->recv(buf, nullptr)) >= 0)
      if (auto p = bam::parse_sap(std::span<const uint8_t>(buf.data(), size_t(n)))) got.push_back(*p);
  }
  // A packet (announcement or deletion) of the SDP's session name, not seen before `from`.
  bool wait_for_packet(bool deletion, const std::string& name, size_t from, bam::SapPacket* out, int ms = 2000) {
    return wait_for(
        [&] {
          poll();
          for (size_t i = from; i < got.size(); ++i) {
            const auto s = bam::parse_sdp(got[i].sdp);
            if (got[i].deletion == deletion && s.stream && s.stream->session_name == name) {
              *out = got[i];
              return true;
            }
          }
          return false;
        },
        ms);
  }
};

be::EncoderEngineOptions sap_options() {
  be::EncoderEngineOptions o = no_ptp();
  o.aes67_require_ptp = false;
  o.enable_sap = true;
  return o;
}

be::EncoderConfig sap_config(const std::string& group, uint16_t port) {
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {aes67_station(group, port)};
  return ready(cfg);
}

}  // namespace

TEST_CASE("AES67 output: announced by SAP with its SDP, withdrawn when disabled and at stop") {
  SapListener sap;
  be::EncoderConfig cfg = sap_config("239.69.170.1", 15270);
  be::EncoderEngine engine(sap_options());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  bam::SapPacket ann;
  REQUIRE(sap.wait_for_packet(false, "Radio AES67", 0, &ann));
  const auto parsed = bam::parse_sdp(ann.sdp);
  REQUIRE(parsed.stream);
  CHECK(parsed.stream->multicast_address == be::aes67_default_address(cfg.stations[0].processing.web_port_slot));
  CHECK(parsed.stream->frames_per_packet == 48);
  CHECK(parsed.stream->ptime_ms == doctest::Approx(1.0));
  CHECK(ann.hash == bam::sap_hash(ann.sdp));
  // disabled: a deletion with the same hash
  size_t from = sap.got.size();
  cfg.stations[0].aes67_output.enabled = false;
  engine.apply_config(cfg);
  bam::SapPacket del;
  REQUIRE(sap.wait_for_packet(true, "Radio AES67", from, &del));
  CHECK(del.hash == ann.hash);
  // enabled again, then stop: announced, then withdrawn
  from = sap.got.size();
  cfg.stations[0].aes67_output.enabled = true;
  engine.apply_config(cfg);
  REQUIRE(sap.wait_for_packet(false, "Radio AES67", from, &ann));
  from = sap.got.size();
  engine.stop();
  REQUIRE(sap.wait_for_packet(true, "Radio AES67", from, &del));
  CHECK(del.hash == ann.hash);
}

TEST_CASE("AES67 output: another device announcing the same address is reported, the address stays") {
  be::EncoderConfig cfg = sap_config("239.69.171.1", 15271);
  be::EncoderEngine engine(sap_options());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  const be::Aes67Status before = aes67_of(engine, "s1");
  CHECK(before.conflict.empty());
  // a foreign device (another origin) announces the same address
  bam::SdpStream f;
  f.session_name = "Obcy nadajnik";
  f.origin_address = "127.0.0.2";
  f.multicast_address = before.address;
  f.ttl = 15;
  f.port = bam::kRtpPort;
  f.payload_type = 98;
  f.encoding = bam::Encoding::L24;
  f.sample_rate = 48000;
  f.channels = 2;
  f.frames_per_packet = 48;
  f.ptime_ms = 1.0;
  const std::string sdp = bam::generate_sdp(f, 0x1234, 1);
  std::string serr;
  auto tx = bp::UdpSocket::open(bam::kSapPort, &serr);
  REQUIRE_MESSAGE(tx, serr);
  tx->set_multicast_out(kLoopback, 1, true);
  const auto pkt = bam::build_sap(false, 0x1111, {127, 0, 0, 2}, sdp);
  REQUIRE(wait_for(
      [&] {
        tx->send_to(pkt, bam::kSapGroup, bam::kSapPort);
        return aes67_of(engine, "s1").conflict.find("Obcy nadajnik") != std::string::npos;
      },
      2000));
  const be::Aes67Status after = aes67_of(engine, "s1");
  CHECK(after.address == before.address);
  CHECK(after.state == be::Aes67State::Sending);
  CHECK(logged("ten sam adres ogłasza"));
  CHECK(after.conflict.find("127.0.0.2") != std::string::npos);
}

TEST_CASE("AES67 output: the SDP") {
  be::EncoderConfig cfg = sap_config("239.69.172.1", 15272);
  cfg.stations[0].aes67_output.ptime_us = 4000;
  cfg.stations[0].aes67_output.address = "239.69.172.9";
  cfg.stations[0].aes67_output.name = "Nazwa SDP";
  cfg = ready(cfg);
  be::EncoderEngine engine(aes67_options(no_ptp()));
  std::string err;
  CHECK_FALSE(engine.aes67_sdp("s1", &err));  // not running
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  const auto sdp = engine.aes67_sdp("s1", &err);
  REQUIRE_MESSAGE(sdp, err);
  const auto parsed = bam::parse_sdp(*sdp);
  REQUIRE_MESSAGE(parsed.stream, parsed.error);
  CHECK(parsed.stream->multicast_address == "239.69.172.9");
  CHECK(parsed.stream->session_name == "Nazwa SDP");
  CHECK(parsed.stream->channels == 2);
  CHECK(parsed.stream->encoding == bam::Encoding::L24);
  CHECK(parsed.stream->frames_per_packet == 192);
  CHECK(parsed.stream->ptime_ms == doctest::Approx(4.0));
  CHECK(sdp->find("a=ts-refclk:") != std::string::npos);
}

namespace {

std::string foreign_sdp(const std::string& name, const std::string& origin, const std::string& address, uint64_t session_id) {
  bam::SdpStream f;
  f.session_name = name;
  f.origin_address = origin;
  f.multicast_address = address;
  f.ttl = 15;
  f.port = bam::kRtpPort;
  f.payload_type = 98;
  f.encoding = bam::Encoding::L24;
  f.sample_rate = 48000;
  f.channels = 2;
  f.frames_per_packet = 48;
  f.ptime_ms = 1.0;
  return bam::generate_sdp(f, session_id, 1);
}

}  // namespace

TEST_CASE("AES67 output: a foreign stream from the same computer (same origin, other session id) is a conflict") {
  be::EncoderConfig cfg = sap_config("239.69.173.1", 15273);
  be::EncoderEngine engine(sap_options());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  const std::string address = aes67_of(engine, "s1").address;
  std::string serr;
  auto tx = bp::UdpSocket::open(bam::kSapPort, &serr);
  REQUIRE_MESSAGE(tx, serr);
  tx->set_multicast_out(kLoopback, 1, true);
  const auto pkt = bam::build_sap(false, 0x2222, {127, 0, 0, 1}, foreign_sdp("Master na tym komputerze", kLoopback, address, 0x1234));
  REQUIRE(wait_for(
      [&] {
        tx->send_to(pkt, bam::kSapGroup, bam::kSapPort);
        return aes67_of(engine, "s1").conflict.find("Master na tym komputerze") != std::string::npos;
      },
      2000));
  CHECK(aes67_of(engine, "s1").conflict.find("127.0.0.1") != std::string::npos);
}

TEST_CASE("AES67 output: its own announcement is not a conflict") {
  SapListener sap;
  be::EncoderConfig cfg = sap_config("239.69.174.1", 15274);
  const size_t warnings = log_lines_with("ten sam adres ogłasza");
  be::EncoderEngine engine(sap_options());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  bam::SapPacket ann;
  REQUIRE(sap.wait_for_packet(false, "Radio AES67", 0, &ann));  // it looped back to every listener
  bp::sleep_ns(1'000'000'000);
  CHECK(aes67_of(engine, "s1").conflict.empty());
  CHECK(log_lines_with("ten sam adres ogłasza") == warnings);
}

TEST_CASE("AES67 output: waiting for PTP, nothing is announced") {
  SapListener sap;
  be::EncoderConfig cfg = sap_config("239.69.175.1", 15275);
  be::EncoderEngineOptions o = sap_options();
  o.aes67_require_ptp = true;  // no PTP master on loopback
  be::EncoderEngine engine(o);
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  REQUIRE(wait_for([&] { return aes67_of(engine, "s1").state == be::Aes67State::WaitingForPtp; }, 2000));
  bam::SapPacket p;
  CHECK_FALSE(sap.wait_for_packet(false, "Radio AES67", 0, &p, 1500));
}

TEST_CASE("AES67 output: without an AES67 output the encoder sends no SAP") {
  SapListener sap;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  cfg.stations = {station("s1", "Radio 1.1")};
  cfg.stations[0].inputs = {};
  be::InputConfig a;
  a.sdp = make_sdp("Wejście", "239.69.176.1", 15276);
  cfg.stations[0].inputs = {a};
  cfg = ready(cfg);
  be::EncoderEngine engine(sap_options());
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  bp::sleep_ns(1'500'000'000);
  sap.poll();
  CHECK(sap.got.empty());
}

TEST_CASE("AES67 output: after an interface change its own old announcement is not a conflict") {
  SapListener sap;
  const std::string sdp1 = foreign_sdp("Własny", kLoopback, "239.69.177.1", 0x42454E01);
  const std::string sdp2 = foreign_sdp("Własny", "127.0.0.2", "239.69.177.1", 0x42454E01);
  be::detail::Aes67Sap own;
  own.set_announcements(kLoopback, {{"s1", sdp1}});
  // the entry exists: seen from another origin it reads as foreign
  REQUIRE(wait_for([&] { return !own.conflict("239.69.177.1", "127.0.0.2").empty(); }, 2000));
  own.set_announcements("127.0.0.2", {{"s1", sdp2}});
  bp::sleep_ns(1'000'000'000);
  CHECK(own.conflict("239.69.177.1", "127.0.0.2").empty());
}
