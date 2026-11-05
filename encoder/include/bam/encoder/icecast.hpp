// SPDX-License-Identifier: GPL-3.0-or-later
// Icecast source client (spec §6): `PUT /<mount> HTTP/1.1` with Basic auth, Content-Type,
// Ice-Public: 0, Ice-Name and Expect: 100-continue, then the encoded stream as a continuous body
// (no Content-Length). A server that refuses PUT with 400/405 is tried again with the old
// `SOURCE /<mount> ICE/1.0` protocol. Lost or refused connections are retried after 1, 2, 5,
// 10 s and then every 10 s.
//
// Threading: every client runs its own thread, which resolves the host, connects and sends.
// push(), on_reconnect() and status() may be called from any thread and never wait on the
// network (they only take a short lock around the queue / status). stop() returns within a few
// hundred milliseconds even when the server is unreachable or stalled.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bam::platform {
class TcpStream;
}

namespace bam::encoder {

struct IcecastTarget {
  std::string host;  // host name or IPv4 address
  uint16_t port = 8000;
  std::string mount;  // "/live" or "live"
  std::string user;   // empty = "source", the Icecast default
  std::string password;
  std::string station_name;  // Ice-Name
  std::string content_type;  // e.g. "audio/mpeg" (codec_content_type)
};

enum class IcecastState { Idle, Connecting, Streaming, Error };

struct IcecastStatus {
  IcecastState state = IcecastState::Idle;
  std::string error;  // Polish user text; the last error, kept while reconnecting, empty when streaming
  uint64_t bytes_sent = 0;     // stream bytes delivered to the socket (all connections)
  uint64_t reconnects = 0;     // connection attempts after the first one
  uint64_t dropped_bytes = 0;  // bytes thrown away because the queue overflowed
  double kbps = 0;             // measured send rate over the last second while streaming
};

// The error texts shown to the user (spec §6).
inline constexpr std::string_view kIcecastErrAuth = "złe hasło lub użytkownik";            // 401
inline constexpr std::string_view kIcecastErrForbidden = "mountpoint zajęty lub zabroniony";  // 403
inline constexpr std::string_view kIcecastErrUnreachable = "serwer nieosiągalny";          // no connection

// Waits before the 1st, 2nd, ... retry; the last value repeats (1, 2, 5, 10 s, then every 10 s).
std::vector<int> icecast_retry_schedule_ms();

namespace testing {
// Test hook: replaces the retry schedule of every client (empty = back to the default).
void set_icecast_retry_schedule_ms(std::vector<int> schedule);
// Test hook: every connect (name lookup included) first waits this long, like a slow DNS server
// or an unanswered SYN (0 = off).
void set_icecast_connect_delay_ms(int ms);
std::string base64(std::string_view data);  // the Basic auth encoder (RFC 4648, with padding)
}  // namespace testing

class IcecastClient {
 public:
  // Starts the client thread, which connects right away. queue_limit_bytes bounds the data
  // waiting to be sent (e.g. 5 s of the stream).
  explicit IcecastClient(IcecastTarget t, size_t queue_limit_bytes);
  ~IcecastClient();  // stop()
  IcecastClient(const IcecastClient&) = delete;
  IcecastClient& operator=(const IcecastClient&) = delete;

  // Queues one piece of the stream (e.g. a codec frame or an Ogg page); pieces are sent whole
  // and in order. When the queue would exceed its limit the oldest pieces are dropped (whole,
  // so frames and pages stay intact) and counted in dropped_bytes; a piece larger than the whole
  // limit is dropped itself. Ignored after stop().
  void push(std::span<const uint8_t> data);

  // Called on the client thread whenever a connection is established that is not the result of
  // the very first attempt (a reconnect after a lost or refused connection), before anything is
  // sent on it. Everything still queued from before is discarded just before the call, since
  // the server expects a fresh stream: the producer should restart its stream on its next
  // push (e.g. reopen the encoder so the next bytes carry a new Ogg/FLAC header); pieces pushed
  // from the callback itself or later are sent. A piece the producer was already pushing while
  // the callback ran can still go out before the new header (one old frame / Ogg page). Keep
  // the callback short and do not call stop() from it.
  void on_reconnect(std::function<void()> cb);

  IcecastStatus status() const;

  // Ends the connection and joins the client thread; idempotent. State becomes Idle.
  void stop();

 private:
  void run();
  // One connection attempt (PUT, or SOURCE after a 400/405); false with `error` set on failure.
  bool open_connection(platform::TcpStream& out, std::string& error);
  bool connect(platform::TcpStream& out, std::string& error);  // also false when stopped
  // Sends the queue until the connection is lost (true, `error` set) or stop() (false).
  bool stream(platform::TcpStream& s, std::string& error);
  void set_state(IcecastState state, std::string error);
  bool wait_retry(int delay_ms);  // false when stopped during the wait
  void discard_queue();

  const IcecastTarget target_;
  const size_t queue_limit_;

  mutable std::mutex m_;  // queue_, queued_bytes_, on_reconnect_ and status_
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> queue_;
  size_t queued_bytes_ = 0;
  std::atomic<bool> stopping_{false};  // written under m_ (no lost wake-up), read anywhere
  bool use_source_ = false;            // client thread only: the server refused PUT
  std::function<void()> on_reconnect_;
  IcecastStatus status_;

  std::mutex stop_m_;  // serializes stop()
  std::thread thread_;
};

}  // namespace bam::encoder
