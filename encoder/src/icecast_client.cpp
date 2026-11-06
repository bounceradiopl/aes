// SPDX-License-Identifier: GPL-3.0-or-later
// Icecast source client (spec §6). One thread per client: connect (host name resolution
// included), PUT or SOURCE handshake, then the queue as a continuous body. Every network wait
// is sliced into short polls that check stopping_, so stop() never waits for a timeout; the one
// call that cannot be sliced, TcpStream::connect() (getaddrinfo + connect), runs on a detached
// helper thread that the client thread waits for, up to an overall deadline.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "bam/encoder/icecast.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"

namespace bam::encoder {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int kConnectTimeoutMs = 5000;    // TCP connect (after the name is resolved)
constexpr int kConnectDeadlineMs = kConnectTimeoutMs + 5000;  // name lookup + connect, overall
constexpr int kHandshakeTimeoutMs = 5000;  // request sent and response head received
constexpr int kRetryResetMs = 10000;       // streaming this long starts the retry schedule over
constexpr int kSendStallMs = 10000;        // no byte accepted by the socket: the server is dead
constexpr int kSliceMs = 50;               // longest wait before stopping_ is checked again
constexpr size_t kMaxResponseHead = 8192;
constexpr const char* kUserAgent = "Bounce Encoder/1.0.0";

constexpr const char* kErrNoName = "nie można znaleźć serwera ";  // + host
constexpr const char* kErrNoAnswer = "serwer nie odpowiada";
constexpr const char* kErrClosed = "serwer zamknął połączenie";
constexpr const char* kErrBadReply = "nieprawidłowa odpowiedź serwera";
constexpr const char* kErrLost = "połączenie przerwane";
constexpr const char* kErrStalled = "serwer nie odbiera danych";

std::mutex g_schedule_m;
std::vector<int> g_schedule_override;  // test hook; empty = the default schedule
std::atomic<int> g_connect_delay_ms{0};  // test hooks (0 = off / the default)
std::atomic<int> g_connect_deadline_ms{0};
std::atomic<int> g_handshake_timeout_ms{0};
std::atomic<int> g_retry_reset_ms{0};

int hook_or(const std::atomic<int>& hook, int fallback) {
  const int v = hook.load();
  return v > 0 ? v : fallback;
}

std::vector<int> retry_schedule() {
  std::lock_guard lk(g_schedule_m);
  return g_schedule_override.empty() ? icecast_retry_schedule_ms() : g_schedule_override;
}

std::string base64_encode(std::string_view in) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (const size_t rest = in.size() - i; rest > 0) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | (rest == 2 ? uint32_t(uint8_t(in[i + 1])) << 8 : 0);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// A header value without control characters (no CR/LF: nothing can inject another header).
std::string header_value(std::string_view v) {
  std::string out;
  for (const char c : v) {
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
  }
  return out;
}

// "/<mount>": one leading slash; whitespace and control characters (which would break the
// request line) are dropped.
std::string mount_path(std::string_view m) {
  while (!m.empty() && m.front() == '/') m.remove_prefix(1);
  std::string out = "/";
  for (const char c : m) {
    if (static_cast<unsigned char>(c) > 0x20 && c != 0x7f) out += c;
  }
  return out;
}

std::string build_request(const IcecastTarget& t, bool source) {
  const std::string user = t.user.empty() ? "source" : t.user;
  std::string r;
  if (source) {
    r = "SOURCE " + mount_path(t.mount) + " ICE/1.0\r\n";
  } else {
    r = "PUT " + mount_path(t.mount) + " HTTP/1.1\r\n";
    r += "Host: " + header_value(t.host) + ":" + std::to_string(t.port) + "\r\n";
  }
  r += "Authorization: Basic " + base64_encode(user + ":" + t.password) + "\r\n";
  r += std::string("User-Agent: ") + kUserAgent + "\r\n";
  if (!t.content_type.empty()) r += "Content-Type: " + header_value(t.content_type) + "\r\n";
  r += "Ice-Name: " + header_value(t.station_name) + "\r\n";
  r += "Ice-Public: 0\r\n";
  if (!source) r += "Expect: 100-continue\r\n";
  r += "\r\n";
  return r;
}

// The status code of "HTTP/1.x NNN ..." or "ICE/1.0 NNN ..."; 0 when there is none.
int parse_status(const std::string& head) {
  const std::string line = head.substr(0, head.find_first_of("\r\n"));
  if (line.rfind("HTTP/", 0) != 0 && line.rfind("ICE/", 0) != 0) return 0;
  const size_t sp = line.find(' ');
  if (sp == std::string::npos || sp + 4 > line.size()) return 0;
  int code = 0;
  for (size_t i = sp + 1; i < sp + 4; ++i) {
    if (line[i] < '0' || line[i] > '9') return 0;
    code = code * 10 + (line[i] - '0');
  }
  return code;
}

std::string refusal_text(int code) {
  switch (code) {
    case 401:
      return std::string(kIcecastErrAuth);
    case 403:
      return std::string(kIcecastErrForbidden);
    default:
      return "serwer odrzucił połączenie (HTTP " + std::to_string(code) + ")";
  }
}

int ms_left(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<milliseconds>(deadline - Clock::now()).count();
  return static_cast<int>(std::clamp<int64_t>(left, 0, kSliceMs));
}

// Sends the request and reads the response head. Returns the status code (> 0), 0 with `error`
// set on failure, or -1 when stopped.
int handshake(platform::TcpStream& s, const std::string& request, const std::atomic<bool>& stopping,
              std::string& error) {
  // One deadline for the whole exchange: a server trickling its reply byte by byte times out too.
  const auto deadline = Clock::now() + milliseconds(hook_or(g_handshake_timeout_ms, kHandshakeTimeoutMs));
  const auto* req = reinterpret_cast<const uint8_t*>(request.data());
  size_t sent = 0;
  while (sent < request.size()) {
    if (stopping) return -1;
    if (Clock::now() >= deadline) {
      error = kErrNoAnswer;
      return 0;
    }
    const int n = s.send_some(std::span(req + sent, request.size() - sent), ms_left(deadline));
    if (n < 0) {
      error = kErrClosed;
      return 0;
    }
    sent += static_cast<size_t>(n);
  }
  std::string head;
  std::vector<uint8_t> buf(1024);
  for (;;) {
    if (head.find("\r\n\r\n") != std::string::npos || head.find("\n\n") != std::string::npos) break;
    if (head.size() > kMaxResponseHead) {
      error = kErrBadReply;
      return 0;
    }
    if (stopping) return -1;
    if (Clock::now() >= deadline) {
      error = kErrNoAnswer;
      return 0;
    }
    const int n = s.recv_some(buf, ms_left(deadline));
    if (n < 0) {
      error = kErrClosed;
      return 0;
    }
    head.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
  }
  const int code = parse_status(head);
  if (code == 0) error = kErrBadReply;
  return code;
}

// Result of one TcpStream::connect() on the helper thread.
struct ConnectJob {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  bool abandoned = false;  // the client stopped waiting (stop() or the deadline)
  std::optional<platform::TcpStream> stream;
  std::string error;
};

}  // namespace

std::vector<int> icecast_retry_schedule_ms() { return {1000, 2000, 5000, 10000}; }

namespace testing {
void set_icecast_retry_schedule_ms(std::vector<int> schedule) {
  std::lock_guard lk(g_schedule_m);
  g_schedule_override = std::move(schedule);
}
void set_icecast_connect_delay_ms(int ms) { g_connect_delay_ms = ms; }
void set_icecast_connect_deadline_ms(int ms) { g_connect_deadline_ms = ms; }
void set_icecast_handshake_timeout_ms(int ms) { g_handshake_timeout_ms = ms; }
void set_icecast_retry_reset_ms(int ms) { g_retry_reset_ms = ms; }
std::string base64(std::string_view data) { return base64_encode(data); }
}  // namespace testing

IcecastClient::IcecastClient(IcecastTarget t, size_t queue_limit_bytes)
    : target_(std::move(t)), queue_limit_(queue_limit_bytes) {
  status_.state = IcecastState::Connecting;
  thread_ = std::thread([this] { run(); });
}

IcecastClient::~IcecastClient() { stop(); }

void IcecastClient::push(std::span<const uint8_t> data, bool stream_start) {
  if (data.empty()) return;
  std::vector<uint8_t> piece(data.begin(), data.end());  // allocated outside the lock
  {
    std::lock_guard lk(m_);
    // Not connected: nobody waits for the data and the next connection needs a new stream
    // anyway, so this is not an overflow and is not counted.
    if (stopping_ || status_.state != IcecastState::Streaming) return;
    if (need_start_) {
      if (!stream_start) return;  // the rest of the old stream: the server waits for a new start
      need_start_ = false;
    }
    if (queued_bytes_ + piece.size() > queue_limit_) {
      // The server cannot keep up: whatever is cut out, the stream after the gap would not
      // decode (an Ogg page sequence or a FLAC stream breaks), so drop everything and restart.
      status_.dropped_bytes += queued_bytes_ + piece.size();
      queue_.clear();
      queued_bytes_ = 0;
      need_start_ = true;
      restart_pending_ = true;
    } else {
      queued_bytes_ += piece.size();
      queue_.push_back(std::move(piece));
    }
  }
  cv_.notify_all();
}

void IcecastClient::on_reconnect(std::function<void()> cb) {
  {
    std::lock_guard lk(m_);
    on_reconnect_ = std::move(cb);
    // A restart still waiting for its start piece (e.g. the first connection came up before
    // this registration) is announced to the new callback as well.
    if (status_.state == IcecastState::Streaming && need_start_) restart_pending_ = true;
  }
  cv_.notify_all();
}

IcecastStatus IcecastClient::status() const {
  std::lock_guard lk(m_);
  return status_;
}

void IcecastClient::stop() {
  std::lock_guard sl(stop_m_);
  {
    std::lock_guard lk(m_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lk(m_);
  queue_.clear();
  queued_bytes_ = 0;
  status_.state = IcecastState::Idle;
  status_.error.clear();
  status_.kbps = 0;
  status_.connected_since_ns = 0;
}

void IcecastClient::set_state(IcecastState state, std::string error) {
  std::lock_guard lk(m_);
  status_.state = state;
  status_.error = std::move(error);
  if (state == IcecastState::Streaming) {
    // A new connection needs a new stream: drop what an old one left, wait for a start piece
    // and let the producer restart (on_reconnect, fired by stream()).
    need_start_ = true;
    restart_pending_ = true;
    status_.connected_since_ns = platform::monotonic_ns();
  } else {
    status_.kbps = 0;
    restart_pending_ = false;
    status_.connected_since_ns = 0;
  }
  queue_.clear();  // not counted as dropped: no connection to send it on
  queued_bytes_ = 0;
}

bool IcecastClient::wait_retry(int delay_ms) {
  std::unique_lock lk(m_);
  return !cv_.wait_for(lk, milliseconds(delay_ms), [this] { return stopping_.load(); });
}

void IcecastClient::fire_restart_if_pending() {
  std::function<void()> cb;
  {
    std::lock_guard lk(m_);
    if (!restart_pending_) return;
    restart_pending_ = false;
    cb = on_reconnect_;
  }
  if (cb) cb();
}

bool IcecastClient::connect(platform::TcpStream& out, std::string& error) {
  auto job = std::make_shared<ConnectJob>();
  auto work = [job, host = target_.host, port = target_.port, delay = g_connect_delay_ms.load()] {
    if (delay > 0) {
      // the test hook's slow lookup; a connect the client gave up on never reaches the server
      std::unique_lock lk(job->m);
      if (job->cv.wait_for(lk, milliseconds(delay), [&] { return job->abandoned; })) return;
    }
    std::string err;
    auto s = platform::TcpStream::connect(host, port, kConnectTimeoutMs, &err);
    std::lock_guard lk(job->m);
    job->stream = std::move(s);
    job->error = std::move(err);
    job->done = true;
    job->cv.notify_all();
  };
  try {
    // Detached: after stop() it finishes on its own (within the connect timeout) and its
    // socket closes with the last reference to the job.
    std::thread(work).detach();
  } catch (const std::system_error&) {
    work();  // no thread available: connect here (stop() then waits for the timeout at worst)
  }
  const auto deadline = Clock::now() + milliseconds(hook_or(g_connect_deadline_ms, kConnectDeadlineMs));
  std::unique_lock lk(job->m);
  while (!job->done) {
    // Stopped, or the name lookup hangs (getaddrinfo has no timeout of its own): give up on
    // the helper, which then finishes on its own.
    const bool timed_out = Clock::now() >= deadline;
    if (stopping_ || timed_out) {
      job->abandoned = true;
      job->cv.notify_all();
      if (timed_out) error = std::string(kIcecastErrUnreachable);
      return false;
    }
    job->cv.wait_for(lk, std::min(milliseconds(kSliceMs), std::chrono::duration_cast<milliseconds>(
                                                              deadline - Clock::now() + milliseconds(1))));
  }
  if (!job->stream) {
    // TcpStream::connect reports a failed name lookup with this prefix (platform/tcp_*.cpp)
    error = job->error.rfind("nie można rozwiązać", 0) == 0 ? kErrNoName + header_value(target_.host)
                                                            : std::string(kIcecastErrUnreachable);
    return false;
  }
  out = std::move(*job->stream);
  return true;
}

bool IcecastClient::open_connection(platform::TcpStream& out, std::string& error) {
  // A server that refuses one protocol with 400/405 gets the other one right away (PUT is
  // Icecast >= 2.4, SOURCE the older servers); the protocol that worked is kept for reconnects.
  for (int pass = 0; pass < 2; ++pass) {
    platform::TcpStream s;
    if (!connect(s, error)) return false;
    const int code = handshake(s, build_request(target_, use_source_), stopping_, error);
    if (code <= 0) return false;  // stopped, or failed with `error` set
    if (code == 100 || (code >= 200 && code < 300)) {
      out = std::move(s);
      return true;
    }
    error = refusal_text(code);
    if (code != 400 && code != 405) return false;
    use_source_ = !use_source_;
  }
  return false;
}

void IcecastClient::run() {
  // Schedule entry for the next wait. It only goes back to 0 after a connection streamed for a
  // while, so a server that accepts and hangs up at once is retried on the full schedule.
  size_t retry = 0;
  while (!stopping_) {
    {
      std::lock_guard lk(m_);
      status_.state = IcecastState::Connecting;  // the last error stays visible while retrying
      status_.kbps = 0;
      status_.connected_since_ns = 0;
    }
    std::string error;
    platform::TcpStream conn;
    const bool connected = open_connection(conn, error);
    if (stopping_) break;
    if (connected) {
      set_state(IcecastState::Streaming, {});  // new stream: on_reconnect fires in stream()
      const auto started = Clock::now();
      if (!stream(conn, error)) break;  // stopped
      if (Clock::now() - started >= milliseconds(hook_or(g_retry_reset_ms, kRetryResetMs))) retry = 0;
    }
    conn.close();
    set_state(IcecastState::Error, error);
    const std::vector<int> schedule = retry_schedule();
    const int delay = schedule[std::min(retry, schedule.size() - 1)];
    ++retry;
    if (!wait_retry(delay)) break;
    std::lock_guard lk(m_);
    ++status_.reconnects;
  }
}

bool IcecastClient::stream(platform::TcpStream& s, std::string& error) {
  std::vector<uint8_t> piece;
  size_t off = 0;
  std::vector<uint8_t> scratch(512);
  auto last_progress = Clock::now();
  auto rate_start = Clock::now();
  uint64_t rate_bytes = 0;
  for (;;) {
    if (stopping_) return false;
    // A new connection or a queue overflow: the producer restarts its stream. Whatever piece
    // is half sent is finished first, so the byte stream stays aligned to whole pieces.
    fire_restart_if_pending();
    const auto now = Clock::now();
    if (now - rate_start >= milliseconds(1000)) {
      const double secs = std::chrono::duration<double>(now - rate_start).count();
      std::lock_guard lk(m_);
      status_.kbps = double(rate_bytes) * 8.0 / 1000.0 / secs;
      rate_start = now;
      rate_bytes = 0;
    }
    if (off == piece.size()) {
      std::unique_lock lk(m_);
      cv_.wait_for(lk, milliseconds(kSliceMs),
                   [this] { return stopping_.load() || !queue_.empty() || restart_pending_; });
      if (stopping_) return false;
      if (!queue_.empty()) {
        piece = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= piece.size();
        off = 0;
        last_progress = Clock::now();
        continue;
      }
      if (restart_pending_) continue;
      lk.unlock();
      // Nothing to send: check whether the server hung up (anything it sends is ignored).
      if (s.recv_some(scratch, 0) < 0) {
        error = kErrLost;
        return true;
      }
      continue;
    }
    const int n = s.send_some(std::span(piece).subspan(off), kSliceMs);
    if (n < 0) {
      error = kErrLost;
      return true;
    }
    if (n == 0) {
      if (Clock::now() - last_progress >= milliseconds(kSendStallMs)) {
        error = kErrStalled;
        return true;
      }
      continue;
    }
    off += static_cast<size_t>(n);
    rate_bytes += static_cast<uint64_t>(n);
    last_progress = Clock::now();
    std::lock_guard lk(m_);
    status_.bytes_sent += static_cast<uint64_t>(n);
  }
}

}  // namespace bam::encoder
