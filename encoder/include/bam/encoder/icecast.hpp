// SPDX-License-Identifier: GPL-3.0-or-later
// Icecast source client (spec §6): `PUT /<mount> HTTP/1.1` with Basic auth, Content-Type,
// Ice-Public: 0, Ice-Name and Expect: 100-continue, then the encoded stream as a continuous body
// (no Content-Length). A server that refuses PUT with 400/405 is tried again with the old
// `SOURCE /<mount> ICE/1.0` protocol. Lost or refused connections are retried after 1, 2, 5,
// 10 s and then every 10 s; the wait goes back to 1 s only after a connection streamed for at
// least 10 s (a server that accepts and hangs up at once is not hammered every second).
//
// Stream-start contract: the server needs every connection to begin with the start of a stream
// (for Ogg/FLAC the header pages). The client therefore only queues data while Streaming, and
// after every transition into Streaming (the first connection included) and after every queue
// overflow it discards its queue, waits for a piece pushed with stream_start = true and fires
// on_reconnect so the producer starts a new stream:
//  - MP3/AAC producers mark every push stream_start (any frame boundary is a valid start);
//  - FLAC/Ogg producers mark the header push after each encoder (re)open, and reopen the
//    encoder when on_reconnect fires.
// on_reconnect runs on the client thread: the producer only sets a flag there and restarts its
// encoder on its own thread before its next push.
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
  uint64_t dropped_bytes = 0;  // bytes thrown away because the queue overflowed while streaming
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
// or an unanswered SYN (0 = off). A connect the client gave up on (stop() or the deadline)
// skips the rest of the delay and never reaches the server.
void set_icecast_connect_delay_ms(int ms);
// Test hooks (0 = back to the default): the overall connect deadline (name lookup + TCP connect,
// default connect timeout + 5 s), the handshake timeout (request sent and response head
// received, default 5 s) and how long a connection must stream before the retry schedule starts
// over (default 10 s).
void set_icecast_connect_deadline_ms(int ms);
void set_icecast_handshake_timeout_ms(int ms);
void set_icecast_retry_reset_ms(int ms);
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
  // and in order. stream_start marks a piece a new stream may begin with (see the contract
  // above). Pieces are discarded, and not counted, while not Streaming (Idle, Connecting,
  // Error) and while the client waits for a stream_start piece. When a piece would push the
  // queue over its limit, the whole queue and the piece are dropped and counted in
  // dropped_bytes, and the stream restarts (wait for stream_start, on_reconnect). Ignored after
  // stop(). Never waits on the network.
  void push(std::span<const uint8_t> data, bool stream_start = false);

  // Called on the client thread whenever the server needs a new stream: on every transition
  // into Streaming (the first connection included) and after a queue overflow, before anything
  // of the new stream is sent; the client then waits for a stream_start piece. Registering a
  // callback while such a restart is still waiting for its start piece fires it for the new
  // callback too, so a connection that came up before the registration is not missed. Called
  // without any client lock held; keep it short (set a flag) and do not call stop() from it.
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
  void fire_restart_if_pending();  // calls on_reconnect_ without m_ when a restart is pending

  const IcecastTarget target_;
  const size_t queue_limit_;

  mutable std::mutex m_;  // queue_, queued_bytes_, need_start_, restart_pending_, on_reconnect_, status_
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> queue_;
  size_t queued_bytes_ = 0;
  bool need_start_ = false;       // pieces are discarded until a stream_start piece arrives
  bool restart_pending_ = false;  // on_reconnect_ still has to be called for this restart
  std::atomic<bool> stopping_{false};  // written under m_ (no lost wake-up), read anywhere
  bool use_source_ = false;            // client thread only: the server refused PUT
  std::function<void()> on_reconnect_;
  IcecastStatus status_;

  std::mutex stop_m_;  // serializes stop()
  std::thread thread_;
};

}  // namespace bam::encoder
