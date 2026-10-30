// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/http_server.hpp"

#include <httplib.h>

#include <atomic>
#include <thread>

#include "bam/auth.hpp"
#include "bam/engine.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "json_api.hpp"

namespace bam {

namespace {
using api::json;

constexpr const char* kCookie = "bam_session";
constexpr int kHttpThreads = 32;  // every open panel tab holds one thread for its SSE feed

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, int status, const std::string& message) {
  send_json(res, {{"error", message}}, status);
}

std::string session_cookie(const httplib::Request& req) {
  const std::string cookies = req.get_header_value("Cookie");
  const std::string key = std::string(kCookie) + "=";
  size_t pos = 0;
  while (pos < cookies.size()) {
    while (pos < cookies.size() && (cookies[pos] == ' ' || cookies[pos] == ';')) ++pos;
    if (cookies.compare(pos, key.size(), key) == 0) {
      const size_t end = cookies.find(';', pos);
      return cookies.substr(pos + key.size(), end == std::string::npos ? std::string::npos : end - pos - key.size());
    }
    const size_t next = cookies.find(';', pos);
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return {};
}

bool is_local(const httplib::Request& req) {
  return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" || req.remote_addr == "::ffff:127.0.0.1";
}

std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (const std::exception&) {
    send_error(res, 400, "nieprawidłowy JSON w treści żądania");
    return std::nullopt;
  }
}
}  // namespace

struct HttpServer::Impl {
  Engine& engine;
  httplib::Server server;
  SessionStore sessions;
  std::thread thread;
  std::atomic<bool> stopping{false};

  explicit Impl(Engine& e) : engine(e) {}

  void set_session_cookie(httplib::Response& res, const std::string& token, bool clear = false) {
    std::string c = std::string(kCookie) + "=" + (clear ? "" : token) + "; Path=/; HttpOnly; SameSite=Strict";
    if (clear) c += "; Max-Age=0";
    res.set_header("Set-Cookie", c);
  }

  bool authorized(const httplib::Request& req) {
    const std::string token = session_cookie(req);
    return !token.empty() && sessions.valid(token, platform::monotonic_ns());
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
    send_json(res, {{"ok", r.ok}, {"issues", api::issues_json(r.issues)}, {"notices", r.notices}, {"config", api::config_json(engine.config())}},
              r.ok ? 200 : 400);
  }

  void routes() {
    server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
      res.set_header("X-Frame-Options", "DENY");
      res.set_header("X-Content-Type-Options", "nosniff");
      res.set_header("Cache-Control", "no-store");
      const bool open = req.path == "/api/health" || req.path == "/api/login" || req.path == "/api/setup" ||
                        req.path == "/api/auth-state" || req.path.rfind("/api/", 0) != 0;
      if (open || authorized(req)) return httplib::Server::HandlerResponse::Unhandled;
      send_error(res, 401, "wymagane logowanie");
      return httplib::Server::HandlerResponse::Handled;
    });

    // ---- public ----
    server.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
      const auto st = engine.status();
      send_json(res, {{"service", "ok"},
                      {"product", kProductName},
                      {"version", st.version},
                      {"ptp", api::ptp_state_code(st)},
                      {"asio_connected", st.asio_connected}});
    });

    server.Get("/api/auth-state", [this](const httplib::Request& req, httplib::Response& res) {
      json logo = nullptr;
      for (size_t i = 0; i < kPanelAssetCount; ++i) {
        const std::string path = kPanelAssets[i].path;
        if (path == "/logo.svg" || (path == "/logo.png" && logo.is_null())) logo = path.substr(1);
      }
      send_json(res, {{"password_set", !engine.config().password_hash.empty()},
                      {"logged_in", authorized(req)},
                      {"local", is_local(req)},
                      {"logo", logo}});
    });

    server.Post("/api/setup", [this](const httplib::Request& req, httplib::Response& res) {
      if (!engine.config().password_hash.empty()) return send_error(res, 409, "hasło jest już ustawione");
      if (!is_local(req)) return send_error(res, 403, "pierwsze hasło można ustawić tylko z tego komputera (127.0.0.1)");
      auto body = parse_body(req, res);
      if (!body) return;
      const std::string pw = body->value("password", "");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      if (!engine.set_password_hash(make_password_hash(pw))) return send_error(res, 500, "nie można zapisać hasła");
      log_info("Ustawiono hasło panelu");
      set_session_cookie(res, sessions.create(platform::monotonic_ns()));
      send_json(res, {{"ok", true}});
    });

    server.Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      const std::string stored = engine.config().password_hash;
      if (stored.empty()) return send_error(res, 409, "hasło nie jest jeszcze ustawione");
      if (!verify_password(body->value("password", ""), stored)) {
        log_warning("Nieudane logowanie do panelu z " + req.remote_addr);
        platform::sleep_ns(500'000'000);  // slows down guessing
        return send_error(res, 401, "nieprawidłowe hasło");
      }
      set_session_cookie(res, sessions.create(platform::monotonic_ns()));
      send_json(res, {{"ok", true}});
    });

    // ---- authorized ----
    server.Post("/api/logout", [this](const httplib::Request& req, httplib::Response& res) {
      sessions.remove(session_cookie(req));
      set_session_cookie(res, "", true);
      send_json(res, {{"ok", true}});
    });

    server.Post("/api/password", [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!verify_password(body->value("current", ""), engine.config().password_hash))
        return send_error(res, 403, "obecne hasło jest nieprawidłowe");
      const std::string pw = body->value("password", "");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      if (!engine.set_password_hash(make_password_hash(pw))) return send_error(res, 500, "nie można zapisać hasła");
      log_info("Zmieniono hasło panelu");
      send_json(res, {{"ok", true}});
    });

    server.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::config_json(engine.config()));
    });

    server.Put("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      json patch = *body;
      if (patch.contains("devices")) {
        // devices are matched by position; merge each one onto the current device
        json current = json::parse(config_to_json(engine.config()));
        json merged = current["devices"];
        const auto& arr = patch["devices"];
        if (!arr.is_array()) return send_error(res, 400, "\"devices\" musi być tablicą");
        for (size_t i = 0; i < arr.size() && i < merged.size(); ++i) {
          json d = arr[i];
          d.erase("index");
          merged[i].merge_patch(d);
        }
        patch["devices"] = merged;
      }
      apply_patch(patch, res);
    });

    server.Get(R"(/api/devices/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak urządzenia");
      send_json(res, api::device_json(engine.config().devices[n - 1], n));
    });

    server.Put(R"(/api/devices/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak urządzenia");
      auto body = parse_body(req, res);
      if (!body) return;
      json current = json::parse(config_to_json(engine.config()));
      json d = *body;
      d.erase("index");
      current["devices"][n - 1].merge_patch(d);
      apply_patch({{"devices", current["devices"]}}, res);
    });

    server.Get(R"(/api/devices/(\d+)/sdp)", [this](const httplib::Request& req, httplib::Response& res) {
      const int n = std::stoi(req.matches[1]);
      if (n < 1 || n > kDeviceCount) return send_error(res, 404, "brak urządzenia");
      res.set_content(engine.output_sdp(n), "application/sdp");
    });

    server.Get("/api/streams", [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::streams_json(engine.discovered_streams()));
    });

    auto status_handler = [this](const httplib::Request&, httplib::Response& res) { send_json(res, api::status_json(engine.status())); };
    server.Get("/api/status", status_handler);
    server.Get("/api/stats", status_handler);

    server.Post("/api/stats/reset", [this](const httplib::Request&, httplib::Response& res) {
      engine.reset_stats();
      send_json(res, {{"ok", true}});
    });

    server.Get("/api/logs", [](const httplib::Request&, httplib::Response& res) {
      send_json(res, api::logs_json(Logger::instance().recent()));
    });

    server.Get("/api/interfaces", [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& i : platform::list_interfaces()) {
        if (i.loopback) continue;
        out.push_back({{"name", i.name}, {"description", i.description}, {"ipv4", i.ipv4}, {"up", i.up}});
      }
      send_json(res, out);
    });

    server.Get("/api/live", [this](const httplib::Request&, httplib::Response& res) {
      res.set_header("X-Accel-Buffering", "no");
      res.set_chunked_content_provider("text/event-stream", [this](size_t, httplib::DataSink& sink) {
        if (stopping) return false;
        const std::string msg = "event: live\ndata: " + api::live_json(engine.status()).dump() + "\n\n";
        if (!sink.write(msg.data(), msg.size())) return false;
        platform::sleep_ns(100'000'000);
        return !stopping.load();
      });
    });

    // ---- embedded panel ----
    server.Get(R"(/(.*))", [](const httplib::Request& req, httplib::Response& res) {
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
  m.server.new_task_queue = [] { return new httplib::ThreadPool(kHttpThreads); };
  m.routes();
  if (!m.server.bind_to_port(bind, port)) {
    if (error) *error = "nie można otworzyć portu panelu " + bind + ":" + std::to_string(port);
    return false;
  }
  m.thread = std::thread([&m] { m.server.listen_after_bind(); });
  log_info("Panel dostępny pod http://" + (bind == "0.0.0.0" ? std::string("127.0.0.1") : bind) + ":" + std::to_string(port));
  return true;
}

void HttpServer::stop() {
  auto& m = *impl_;
  if (!m.thread.joinable()) return;
  m.stopping = true;
  m.server.stop();
  m.thread.join();
}

}  // namespace bam
