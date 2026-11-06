// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/http_server.hpp"

#include <httplib.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "bam/auth.hpp"
#include "bam/engine.hpp"
#include "bam/http/guard.hpp"
#include "bam/http/login_throttle.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "json_api.hpp"

namespace bam {

namespace {
using api::json;

constexpr const char* kCookie = "bam_session";
constexpr int kHttpThreads = 32;  // every open panel tab holds one thread for its SSE feed

using http::send_error;
using http::send_json;

std::string session_cookie(const httplib::Request& req) { return http::session_cookie(req, kCookie); }

std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (const std::exception&) {
    send_error(res, 400, "nieprawidłowy JSON w treści żądania");
    return std::nullopt;
  }
}

// A text field of a request body; "" when missing or not text.
std::string text(const json& body, const char* key) {
  if (!body.is_object()) return {};
  const auto it = body.find(key);
  return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::atomic<int64_t> g_clock_offset_ns{0};  // testing::set_http_clock_offset_ns
std::mutex g_hook_mutex;
std::function<void()> g_login_verified_hook;  // testing::set_login_verified_hook

int64_t now_ns() { return platform::monotonic_ns() + g_clock_offset_ns.load(); }
}  // namespace

struct HttpServer::Impl {
  Engine& engine;
  httplib::Server server;
  std::mutex sessions_mutex;
  std::shared_ptr<SessionStore> sessions_ = std::make_shared<SessionStore>();  // replaced on a password change
  uint64_t sessions_generation_ = 0;  // counts the replacements (sessions_mutex)
  http::LoginThrottle throttle;
  std::mutex password_mutex;  // the first password and password changes, one at a time
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::atomic<bool> listen_returned{false};  // the listen thread is past listen_after_bind()
  uint16_t port = 0;

  explicit Impl(Engine& e) : engine(e) {}

  std::shared_ptr<SessionStore> sessions() {
    std::lock_guard lock(sessions_mutex);
    return sessions_;
  }

  uint64_t sessions_generation() {
    std::lock_guard lock(sessions_mutex);
    return sessions_generation_;
  }

  // A new session, unless the sessions were restarted (password change) since `generation` was
  // read: a login that checked the old password must not outlive the change. Empty = refused.
  std::string create_session(uint64_t generation) {
    std::lock_guard lock(sessions_mutex);
    if (generation != sessions_generation_) return {};
    return sessions_->create(now_ns());
  }

  // Every session ends except a new one, returned (its token), for whoever changed the password.
  std::string restart_sessions() {
    auto fresh = std::make_shared<SessionStore>();
    std::string token = fresh->create(now_ns());
    std::lock_guard lock(sessions_mutex);
    sessions_ = std::move(fresh);
    ++sessions_generation_;
    return token;
  }

  enum class PasswordResult { Saved, AlreadySet, SaveFailed };
  // `first`: only when no password is set yet, checked under the lock (two first-time setups
  // cannot both win).
  PasswordResult set_password_hash(const std::string& hash, bool first) {
    std::lock_guard lock(password_mutex);
    if (first && !engine.config().password_hash.empty()) return PasswordResult::AlreadySet;
    return engine.set_password_hash(hash) ? PasswordResult::Saved : PasswordResult::SaveFailed;
  }

  void set_session_cookie(httplib::Response& res, const std::string& token, bool clear = false) {
    std::string c = std::string(kCookie) + "=" + (clear ? "" : token) + "; Path=/; HttpOnly; SameSite=Strict";
    if (clear) c += "; Max-Age=0";
    res.set_header("Set-Cookie", c);
  }

  bool authorized(const httplib::Request& req) {
    const std::string token = session_cookie(req);
    return !token.empty() && sessions()->valid(token, now_ns());
  }

  // Applies a JSON merge patch on top of the current configuration.
  void apply_patch(const json& patch, httplib::Response& res) {
    json current = json::parse(config_to_json(engine.config()));
    current.merge_patch(patch);
    current.erase("password_hash");
    std::string err;
    auto cfg = config_from_json(current.dump(), &err);
    if (!cfg) {
      send_error(res, 400, err);
      return;
    }
    const ApplyResult r = engine.apply_config(*cfg);
    json out = api::result_json(r);
    out["config"] = api::config_json(engine.config());
    send_json(res, out, r.ok ? 200 : 400);
  }

  using Access = http::Access;
  static constexpr Access kPublic = Access::Public;
  static constexpr Access kSession = Access::Session;

  // The body type (415) and the session (kSession, 401), checked after the body was read; the
  // handler runs only when both pass (bam/http/guard.hpp).
  httplib::Server::Handler guard(Access access, httplib::Server::Handler handler) {
    return http::guard(access, [this](const httplib::Request& req) { return authorized(req); }, std::move(handler));
  }

  void routes() {
    // One request per connection, the request allowlist on the headers (before any body is read)
    // and JSON answers for httplib's own refusals: bam/http/guard.hpp.
    http::install_request_guard(server);

    // Every route goes through guard(): the body type (415) and the session (kSession, 401), both
    // checked after the body was read.
    using Handler = httplib::Server::Handler;
    auto get = [&](const std::string& pattern, Access a, Handler h) { server.Get(pattern, guard(a, std::move(h))); };
    auto post = [&](const std::string& pattern, Access a, Handler h) { server.Post(pattern, guard(a, std::move(h))); };
    auto put = [&](const std::string& pattern, Access a, Handler h) { server.Put(pattern, guard(a, std::move(h))); };
    auto del = [&](const std::string& pattern, Access a, Handler h) { server.Delete(pattern, guard(a, std::move(h))); };

    // ---- public ----
    get("/api/health", kPublic, [this](const httplib::Request&, httplib::Response& res) {
      const auto st = engine.status();
      send_json(res, {{"service", "ok"},
                      {"product", kProductName},
                      {"version", st.version},
                      {"station_name", st.station_name},
                      {"ptp", api::ptp_state_code(st)},
                      {"ptp_role", ptp_role_name(st.ptp_role)},
                      {"asio_connected", st.asio_connected},
                      {"bridge_connected", st.bridge_connected}});
    });

    get("/api/auth-state", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      json logo = nullptr;
      for (size_t i = 0; i < kPanelAssetCount; ++i) {
        const std::string path = kPanelAssets[i].path;
        if (path == "/logo.svg" || (path == "/logo.png" && logo.is_null())) logo = path.substr(1);
      }
      send_json(res, {{"password_set", !engine.config().password_hash.empty()},
                      {"logged_in", authorized(req)},
                      {"local", http::is_local(req)},  // the address and a loopback Host, as /api/setup checks
                      {"logo", logo}});
    });

    post("/api/setup", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      if (!engine.config().password_hash.empty()) return send_error(res, 409, "hasło jest już ustawione");
      if (!http::is_local_address(req))
        return send_error(res, 403, "pierwsze hasło można ustawić tylko z tego komputera (127.0.0.1)");
      if (!http::is_loopback_host(req))
        return send_error(res, 403,
                          "pierwsze hasło można ustawić tylko z panelu otwartego pod adresem http://127.0.0.1 lub http://localhost "
                          "(nie przez nazwę komputera ani inną nazwę domeny)");
      auto body = parse_body(req, res);
      if (!body) return;
      const std::string pw = text(*body, "password");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      switch (set_password_hash(make_password_hash(pw), true)) {
        case PasswordResult::AlreadySet: return send_error(res, 409, "hasło jest już ustawione");
        case PasswordResult::SaveFailed: return send_error(res, 500, "nie można zapisać hasła");
        case PasswordResult::Saved: break;
      }
      log_info("Ustawiono hasło panelu");
      set_session_cookie(res, sessions()->create(now_ns()));
      send_json(res, {{"ok", true}});
    });

    post("/api/login", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      const uint64_t generation = sessions_generation();  // before the hash: see create_session
      const std::string stored = engine.config().password_hash;
      if (stored.empty()) return send_error(res, 409, "hasło nie jest jeszcze ustawione");
      // guessing is limited by counting failures (429), not by holding a request thread
      if (!throttle.allowed(req.remote_addr, now_ns())) return send_error(res, 429, "zbyt wiele nieudanych prób, spróbuj za chwilę");
      if (!verify_password(text(*body, "password"), stored)) {
        throttle.failed(req.remote_addr, now_ns());
        log_warning("Nieudane logowanie do panelu z " + req.remote_addr);
        return send_error(res, 401, "nieprawidłowe hasło");
      }
      throttle.succeeded(req.remote_addr);
      {
        std::lock_guard hook(g_hook_mutex);
        if (g_login_verified_hook) g_login_verified_hook();
      }
      const std::string token = create_session(generation);
      if (token.empty()) return send_error(res, 401, "hasło zostało właśnie zmienione, zaloguj się ponownie");
      set_session_cookie(res, token);
      send_json(res, {{"ok", true}});
    });

    // ---- authorized ----
    post("/api/logout", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      sessions()->remove(session_cookie(req));
      set_session_cookie(res, "", true);
      send_json(res, {{"ok", true}});
    });

    post("/api/password", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      if (!verify_password(text(*body, "current"), engine.config().password_hash))
        return send_error(res, 403, "obecne hasło jest nieprawidłowe");
      const std::string pw = text(*body, "password");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      if (set_password_hash(make_password_hash(pw), false) != PasswordResult::Saved)
        return send_error(res, 500, "nie można zapisać hasła");
      // every other session (other tabs, other computers) ends; this one goes on with a new cookie
      set_session_cookie(res, restart_sessions());
      log_info("Zmieniono hasło panelu");
      send_json(res, {{"ok", true}});
    });

    get("/api/config", kSession, [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::config_json(engine.config()));
    });

    put("/api/config", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      json patch = *body;
      patch.erase("version");
      // Mixes change only through PUT /api/mixes and PUT /api/routing: a configuration draft the panel
      // took earlier would otherwise put back gains and mutes changed since then (or a removed mix).
      patch.erase("mixes");
      // outputs, subscriptions and asio_names are matched by position; each entry is merged onto
      // the current one, so a client may send only the entries (and fields) it changes.
      json current = json::parse(config_to_json(engine.config()));
      for (const char* key : {"outputs", "subscriptions", "asio_names"}) {
        if (!patch.contains(key)) continue;
        const auto& arr = patch[key];
        if (!arr.is_array()) return send_error(res, 400, std::string("\"") + key + "\" musi być tablicą");
        json merged = current[key];
        for (size_t i = 0; i < arr.size() && i < merged.size(); ++i) {
          json item = arr[i];
          if (item.is_null()) continue;
          if (item.is_object() && merged[i].is_object()) {
            item.erase("index");
            merged[i].merge_patch(item);
          } else {
            merged[i] = item;
          }
        }
        patch[key] = merged;
      }
      apply_patch(patch, res);
    });

    auto output_sdp = [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak wyjścia");
      res.set_content(engine.output_sdp(n), "application/sdp");
    };
    get(R"(/api/outputs/(\d+)/sdp)", kSession, output_sdp);
    get(R"(/api/devices/(\d+)/sdp)", kSession, output_sdp);  // 1.0 path

    // "Nadawaj do urządzenia…": a new output with its source, answered with its SDP
    post("/api/outputs", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      NewOutput o;
      auto text = [&](const char* key, std::string& out) {
        if (body->contains(key) && (*body)[key].is_string()) out = (*body)[key].get<std::string>();
      };
      text("name", o.name);
      text("source", o.source);
      text("address", o.address);
      if (body->contains("ptime_us") && (*body)["ptime_us"].is_number_unsigned()) o.ptime_us = (*body)["ptime_us"].get<uint32_t>();
      const ApplyResult r = engine.add_output(o);
      json out = api::result_json(r);
      if (r.ok) {
        out["config"] = api::config_json(engine.config());
        out["routing"] = api::routing_json(engine.routing());
        out["sdp"] = engine.output_sdp(r.created);
      }
      send_json(res, out, r.ok ? 200 : 400);
    });

    del(R"(/api/outputs/(\d+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak wyjścia");
      const ApplyResult r = engine.remove_output(n);
      json out = api::result_json(r);
      if (r.ok) {
        out["config"] = api::config_json(engine.config());
        out["routing"] = api::routing_json(engine.routing());
      }
      send_json(res, out, r.ok ? 200 : 400);
    });

    get("/api/routing", kSession, [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::routing_json(engine.routing()));
    });

    put("/api/routing", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object() || !body->contains("dst") || !(*body)["dst"].is_string())
        return send_error(res, 400, "oczekiwano {\"dst\": \"...\", \"src\": \"...\" | null}");
      std::optional<std::string> src;
      if (body->contains("src") && !(*body)["src"].is_null()) {
        if (!(*body)["src"].is_string()) return send_error(res, 400, "\"src\" musi być tekstem albo null");
        src = (*body)["src"].get<std::string>();
      }
      const ApplyResult r = engine.set_route((*body)["dst"].get<std::string>(), src);
      json out = api::result_json(r);
      if (r.ok) out["routing"] = api::routing_json(engine.routing());
      send_json(res, out, r.ok ? 200 : 400);
    });

    put("/api/mixes", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object() || !body->contains("dst") || !(*body)["dst"].is_string() || !body->contains("enabled") ||
          !(*body)["enabled"].is_boolean())
        return send_error(res, 400, "oczekiwano {\"dst\": \"...\", \"enabled\": true|false}");
      std::optional<MixConfig> mix;
      if (body->contains("sources")) {
        if (!(*body)["sources"].is_array()) return send_error(res, 400, "\"sources\" musi być listą");
        MixConfig m;
        for (const auto& sj : (*body)["sources"]) {
          if (!sj.is_object() || !sj.contains("source") || !sj["source"].is_string()) return send_error(res, 400, "nieprawidłowe źródło miksu");
          MixSource s;
          s.source = sj["source"].get<std::string>();
          if (sj.contains("gain_db")) {
            if (!sj["gain_db"].is_number()) return send_error(res, 400, "\"gain_db\" źródła miksu musi być liczbą (dB)");
            s.gain_db = sj["gain_db"].get<double>();
          }
          if (sj.contains("mute")) {
            if (!sj["mute"].is_boolean()) return send_error(res, 400, "\"mute\" źródła miksu musi być wartością true albo false");
            s.mute = sj["mute"].get<bool>();
          }
          m.sources.push_back(std::move(s));
        }
        mix = std::move(m);
      }
      std::optional<std::string> keep;
      if (body->contains("keep") && (*body)["keep"].is_string()) keep = (*body)["keep"].get<std::string>();
      const ApplyResult r = engine.set_mix((*body)["dst"].get<std::string>(), (*body)["enabled"].get<bool>(), mix, keep);
      json out = api::result_json(r);
      if (r.ok) out["routing"] = api::routing_json(engine.routing());
      send_json(res, out, r.ok ? 200 : 400);
    });

    post("/api/subscriptions", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      std::string what;
      if (body->is_object() && body->contains("sdp") && (*body)["sdp"].is_string()) {
        what = (*body)["sdp"].get<std::string>();
        if (what.rfind("v=", 0) != 0) return send_error(res, 400, "SDP musi zaczynać się od \"v=\"");
      } else if (body->is_object() && body->contains("sap_id") && (*body)["sap_id"].is_string()) {
        what = (*body)["sap_id"].get<std::string>();
        if (what.rfind("v=", 0) == 0) return send_error(res, 400, "nieprawidłowy identyfikator strumienia SAP");
      } else {
        return send_error(res, 400, "oczekiwano {\"sdp\": \"...\"} albo {\"sap_id\": \"...\"}");
      }
      const ApplyResult r = engine.add_subscription(what);
      json out = api::result_json(r);
      if (r.ok) out["config"] = api::config_json(engine.config());
      send_json(res, out, r.ok ? 200 : 400);
    });

    del(R"(/api/subscriptions/(\d+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak subskrypcji");
      const ApplyResult r = engine.remove_subscription(n);
      json out = api::result_json(r);
      if (r.ok) {
        out["config"] = api::config_json(engine.config());
        out["routing"] = api::routing_json(engine.routing());
      }
      send_json(res, out, r.ok ? 200 : 400);
    });

    get("/api/streams", kSession, [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::streams_json(engine.discovered_streams()));
    });

    auto status_handler = [this](const httplib::Request&, httplib::Response& res) { send_json(res, api::status_json(engine.status())); };
    get("/api/status", kSession, status_handler);
    get("/api/stats", kSession, status_handler);

    post("/api/stats/reset", kSession, [this](const httplib::Request&, httplib::Response& res) {
      engine.reset_stats();
      send_json(res, {{"ok", true}});
    });

    get("/api/logs", kSession, [](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::logs_json(Logger::instance().recent()));
    });

    get("/api/interfaces", kSession, [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& i : platform::list_interfaces()) {
        if (i.loopback) continue;
        out.push_back({{"name", i.name}, {"description", i.description}, {"ipv4", i.ipv4}, {"up", i.up}});
      }
      send_json(res, out);
    });

    // Server-sent events 10 times a second (meters). The session is checked before every event:
    // logout, expiry or a password change ends the feed.
    get("/api/live", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      res.set_header("X-Accel-Buffering", "no");
      res.set_chunked_content_provider("text/event-stream", [this, token = session_cookie(req)](size_t, httplib::DataSink& sink) {
        if (stopping || !sessions()->alive(token, now_ns())) return false;  // alive: an open feed does not extend the session
        const std::string msg = "event: live\ndata: " + api::live_json(engine.status()).dump() + "\n\n";
        if (!sink.write(msg.data(), msg.size())) return false;
        platform::sleep_ns(100'000'000);
        return !stopping.load();
      });
    });

    // ---- embedded panel ----
    get(R"(/(.*))", kPublic, [](const httplib::Request& req, httplib::Response& res) {
      std::string path = req.path == "/" ? "/index.html" : req.path;
      for (size_t i = 0; i < kPanelAssetCount; ++i) {
        if (path == kPanelAssets[i].path) {
          res.set_header("Content-Security-Policy",
                         "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'");
          res.set_content(reinterpret_cast<const char*>(kPanelAssets[i].data), kPanelAssets[i].size, kPanelAssets[i].mime);
          return;
        }
      }
      res.status = 404;
      res.set_content("Nie znaleziono", "text/plain; charset=utf-8");
    });
  }
};

HttpServer::HttpServer(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const std::string& bind, uint16_t port, std::string* error) {
  auto& m = *impl_;
  if (m.thread.joinable()) {
    if (error) *error = "panel już działa";
    return false;
  }
  m.server.new_task_queue = [] { return new httplib::ThreadPool(kHttpThreads); };
  m.routes();
  const int bound = port == 0 ? m.server.bind_to_any_port(bind) : (m.server.bind_to_port(bind, port) ? port : -1);
  if (bound <= 0) {
    if (error) *error = "nie można otworzyć portu panelu " + bind + ":" + std::to_string(port);
    return false;
  }
  m.port = static_cast<uint16_t>(bound);
  m.listen_returned = false;
  m.thread = std::thread([&m] {
    m.server.listen_after_bind();
    m.listen_returned = true;
  });
  http::wait_listening(m.server, m.listen_returned);  // a stop() right after this start must reach a running server
  log_info("Panel dostępny pod http://" + (bind == "0.0.0.0" ? std::string("127.0.0.1") : bind) + ":" + std::to_string(m.port));
  return true;
}

void HttpServer::stop() {
  auto& m = *impl_;
  if (!m.thread.joinable()) return;
  m.stopping = true;
  http::wait_listening(m.server, m.listen_returned);  // httplib ignores a stop() before it listens
  m.server.stop();
  m.thread.join();
}

uint16_t HttpServer::port() const { return impl_->port; }

namespace testing {
void set_http_clock_offset_ns(int64_t offset_ns) { g_clock_offset_ns = offset_ns; }
void set_login_verified_hook(std::function<void()> hook) {
  std::lock_guard lock(g_hook_mutex);
  g_login_verified_hook = std::move(hook);
}
}  // namespace testing

}  // namespace bam
