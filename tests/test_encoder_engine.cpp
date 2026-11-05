// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine end to end (spec §9 "Cały tor"): test AES67 senders on loopback multicast
// (127.0.0.1 only) -> inputs from SDP -> 2 s delay and a -6 dB second input -> station mix ->
// FLAC 48 kHz -> a fake Icecast server on 127.0.0.1 -> decoded bit for bit against the expected
// mix. Also: editing one stream restarts only that stream (the other keeps its connection),
// disabling the station closes its connections, and the PTP slave follows a BounceAES Master
// engine acting as PTP master on loopback (domain 87). Elsewhere PTP stays off (free-run).
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bam/encoder/engine.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "bam/rtp.hpp"
#include "bam/sdp.hpp"
#include "../encoder/src/engine_internal.hpp"  // AudioQueue (the station -> stream handover)

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

struct FakeIcecast {
  struct Conn {
    int id = 0;
    std::string mount;
    std::mutex m;
    std::vector<uint8_t> body;
    std::atomic<size_t> bytes{0};
    std::atomic<bool> open{true};
  };

  bp::TcpListener listener;
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

  void accept_loop() {
    int next_id = 1;
    while (!stop) {
      auto s = listener.accept(50);
      if (!s) continue;
      auto c = std::make_shared<Conn>();
      c->id = next_id++;
      {
        std::lock_guard lock(m);
        conns.push_back(c);
      }
      serving.emplace_back([this, c, s = std::move(*s)]() mutable { serve(*c, s); });
    }
  }

  void serve(Conn& c, bp::TcpStream& s) {
    std::string head;
    std::vector<uint8_t> chunk(65536);
    size_t end = std::string::npos;
    while (!stop && end == std::string::npos) {
      const int n = s.recv_some(chunk, 50);
      if (n < 0) {
        c.open = false;
        return;
      }
      head.append(reinterpret_cast<const char*>(chunk.data()), size_t(n));
      end = head.find("\r\n\r\n");
    }
    if (end == std::string::npos) return;
    {
      // "PUT /mount HTTP/1.1"
      const size_t a = head.find(' '), b = head.find(' ', a + 1);
      std::lock_guard lock(c.m);
      c.mount = head.substr(a + 1, b - a - 1);
      c.body.assign(head.begin() + std::ptrdiff_t(end + 4), head.end());
      c.bytes = c.body.size();
    }
    const std::string ok = "HTTP/1.1 200 OK\r\n\r\n";
    if (!s.send_all(std::span(reinterpret_cast<const uint8_t*>(ok.data()), ok.size()), 2000)) {
      c.open = false;
      return;
    }
    while (!stop) {
      const int n = s.recv_some(chunk, 50);
      if (n < 0) break;
      std::lock_guard lock(c.m);
      c.body.insert(c.body.end(), chunk.begin(), chunk.begin() + n);
      c.bytes = c.body.size();
    }
    c.open = false;
  }

  // The newest connection to `mount`, or nullptr.
  std::shared_ptr<Conn> latest(const std::string& mount) {
    std::lock_guard lock(m);
    for (auto it = conns.rbegin(); it != conns.rend(); ++it) {
      std::lock_guard l2((*it)->m);
      if ((*it)->mount == mount) return *it;
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
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Test";
  // a signal on the input (Ogg FLAC sends digital silence only in pages of ~20 s)
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
  auto streaming = [&](const std::string& id) {
    const be::EncoderStatus now = engine.status();
    if (now.stations.empty()) return false;
    for (const auto& s : now.stations[0].streams)
      if (s.id == id) return s.icecast.state == be::IcecastState::Streaming && s.icecast.bytes_sent > 0;
    return false;
  };
  REQUIRE(wait_for([&] { return streaming("str1") && streaming("str2"); }, 10000));
  auto a1 = server.latest("/a.mp3");
  auto b1 = server.latest("/b.ogg");
  REQUIRE(a1);
  REQUIRE(b1);
  REQUIRE_MESSAGE(wait_for([&] { return a1->bytes > 1000 && b1->bytes > 1000; }, 10000),
                  "a: " << a1->bytes.load() << " bytes, b: " << b1->bytes.load() << " bytes");
  const be::EncoderStatus before = engine.status();
  CHECK_FALSE(before.stations[0].streams[0].codec_library.empty());  // libmp3lame's path
  CHECK(before.stations[0].streams[1].codec_library.empty());        // FLAC is built in

  // str1: 128 -> 192 kb/s; str2: only its gain changes; the input: gain and delay (all live)
  cfg.stations[0].streams[0].bitrate_kbps = 192;
  cfg.stations[0].streams[1].gain_db = -3.0;
  cfg.stations[0].inputs[0].gain_db = -10.0;
  cfg.stations[0].inputs[0].delay_s = 0.5;
  engine.apply_config(ready(cfg));

  REQUIRE(wait_for([&] {
    auto a2 = server.latest("/a.mp3");
    return a2 && a2->id != a1->id && a2->bytes > 1000;
  }, 10000));
  CHECK(wait_for([&] { return !a1->open; }, 5000));  // the old str1 connection was closed
  const size_t b_bytes = b1->bytes;
  CHECK(wait_for([&] { return b1->bytes > b_bytes + 1000; }, 5000));  // str2 keeps sending
  CHECK(b1->open);
  CHECK(server.latest("/b.ogg")->id == b1->id);  // on the same connection
  const be::EncoderStatus after = engine.status();
  CHECK(after.stations[0].started_ns == before.stations[0].started_ns);                // station not restarted
  CHECK(after.stations[0].streams[1].started_ns == before.stations[0].streams[1].started_ns);  // str2 neither
  CHECK(after.stations[0].streams[0].started_ns != before.stations[0].streams[0].started_ns);
  CHECK(after.stations[0].streams[0].bitrate_kbps == 192);
  CHECK(after.stations[0].streams[1].icecast.reconnects == 0);
  // the input kept its socket and RxStream: its counters went on
  CHECK(after.stations[0].inputs[0].rx.packets > before.stations[0].inputs[0].rx.packets);
  CHECK(after.stations[0].inputs[0].gain_db == -10.0);
  CHECK(after.stations[0].inputs[0].delay_s == 0.5);

  // disabling the station closes both connections
  cfg.stations[0].enabled = false;
  engine.apply_config(ready(cfg));
  CHECK(wait_for([&] { return !b1->open && !server.latest("/a.mp3")->open; }, 5000));
  const be::EncoderStatus off = engine.status();
  REQUIRE(off.stations.size() == 1);
  CHECK_FALSE(off.stations[0].running);
  CHECK(off.stations[0].streams[0].icecast.state == be::IcecastState::Idle);
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
  REQUIRE(wait_for([&] { return server.latest("/b.ogg") && server.latest("/b.ogg")->bytes > 10000; }, 10000));
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

TEST_CASE("encoder engine: the PTP slave follows a master on loopback") {
  // A BounceAES Master engine on 127.0.0.1 is the PTP master, on a domain nothing else uses.
  constexpr int kDomain = 87;
  struct Master {
    std::string suffix = std::to_string(bp::current_pid()) + "." + std::to_string(bp::monotonic_ns() % 100000);
    bam::EngineOptions opts;
    std::unique_ptr<bam::Engine> engine;
    Master() {
      opts.config_path = "bam_enc_ptp_" + suffix + ".json";
      opts.shm_name = "bamtest.encptp." + suffix;
      opts.event_name = "bamtest.encptpev." + suffix;
      opts.enable_sap = false;
      auto cfg = bam::default_config();
      cfg.interface_ip = kLoopback;
      cfg.station_name = "PTP";
      cfg.ptp_domain = kDomain;
      cfg.ptp_role = bam::PtpRole::Preferred;
      std::ofstream(opts.config_path) << bam::config_to_json(cfg);
      engine = std::make_unique<bam::Engine>(opts);
    }
    ~Master() {
      engine->stop();
      std::remove(opts.config_path.c_str());
      std::remove((opts.config_path + ".v1.bak").c_str());
      bp::SharedMemory::remove(opts.shm_name);
      bp::NamedEvent::remove(opts.event_name);
    }
  } master;
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

#endif  // BAM_WITH_CODECS
