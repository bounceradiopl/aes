// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/http/guard.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

namespace bam::http {

void send_json(httplib::Response& res, const nlohmann::ordered_json& j, int status) {
  res.status = status;
  res.set_content(j.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, int status, const std::string& message) { send_json(res, {{"error", message}}, status); }

std::string session_cookie(const httplib::Request& req, const std::string& name) {
  const std::string cookies = req.get_header_value("Cookie");
  const std::string key = name + "=";
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

bool is_local_address(const httplib::Request& req) {
  return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" || req.remote_addr == "::ffff:127.0.0.1";
}

bool is_loopback_host(const httplib::Request& req) {
  if (req.get_header_value_count("Host") != 1) return false;
  std::string host = req.get_header_value("Host");
  for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* name : {"127.0.0.1", "localhost", "[::1]"}) {
    const size_t n = std::strlen(name);
    if (host.compare(0, n, name) != 0) continue;
    if (host.size() == n) return true;
    const std::string port = host.substr(n + 1);
    return host[n] == ':' && !port.empty() && port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos;
  }
  return false;
}

bool is_json_type(const std::string& value) {
  auto trim_lower = [](std::string t) {
    const size_t b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
    t = b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
  };
  size_t pos = value.find(';');
  if (trim_lower(value.substr(0, pos)) != "application/json") return false;
  while (pos != std::string::npos) {
    const size_t next = value.find(';', pos + 1);
    const std::string param = trim_lower(value.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1));
    if (param.rfind("charset=", 0) != 0) return false;
    pos = next;
  }
  return true;
}

std::string json_shape_error(std::string_view text) {
  size_t depth = 0, containers = 0, values = 0;
  std::array<size_t, kMaxJsonDepth + 1> members{};  // members of the open object at each depth
  bool in_string = false, escaped = false;
  for (const char ch : text) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == '[' || ch == '{') {
      if (++depth > kMaxJsonDepth)
        return "JSON w treści żądania jest zbyt zagnieżdżony (najwyżej " + std::to_string(kMaxJsonDepth) + " poziomów)";
      if (++containers > kMaxJsonContainers)
        return "JSON w treści żądania ma zbyt wiele obiektów i tablic (najwyżej " + std::to_string(kMaxJsonContainers) + ")";
      members[depth] = 0;
    } else if ((ch == ']' || ch == '}') && depth > 0) {
      --depth;  // a malformed body may close more than it opened: the parser refuses it
    } else if (ch == ',' || ch == ':') {
      // each separates or names a value: a flat list or a wide object costs memory, and an object
      // with many keys is parsed in quadratic time (nlohmann::ordered_json looks every key up)
      if (++values > kMaxJsonValues)
        return "JSON w treści żądania ma zbyt wiele wartości (najwyżej " + std::to_string(kMaxJsonValues) + ")";
      if (ch == ':' && ++members[depth] > kMaxJsonMembers)
        return "obiekt w treści żądania ma zbyt wiele pól (najwyżej " + std::to_string(kMaxJsonMembers) + ")";
    }
  }
  return {};
}

std::optional<nlohmann::ordered_json> parse_json_body(const httplib::Request& req, httplib::Response& res) {
  if (const std::string why = json_shape_error(req.body); !why.empty()) {
    send_error(res, 400, why);
    return std::nullopt;
  }
  try {
    return nlohmann::ordered_json::parse(req.body);
  } catch (const std::exception&) {
    send_error(res, 400, "nieprawidłowy JSON w treści żądania");
    return std::nullopt;
  }
}

// The request allowlist, checked on the headers before httplib reads anything else: only what
// cannot be read safely is refused here. GET, POST, PUT and DELETE only; no Content-Encoding; no
// Range on the API; a body only with one plain Content-Length of at most `max_body_bytes` (no
// chunked body, nothing httplib would read until the connection closes); no multipart body
// (httplib would parse it before any handler). Everything with a bounded body is answered after
// httplib has read that body (guard(): JSON type, session): a server that closes on unread bytes
// makes Windows reset the connection, and the reset destroys the answer before the client can
// read it.
bool refuse(const httplib::Request& req, httplib::Response& res, size_t max_body_bytes) {
  const std::string& m = req.method;
  if (m != "GET" && m != "POST" && m != "PUT" && m != "DELETE") {
    send_error(res, 405, "metoda " + m + " nie jest obsługiwana");
    return true;
  }
  if (req.has_header("Content-Encoding")) {
    send_error(res, 415, "kodowanie treści (Content-Encoding) nie jest obsługiwane");
    return true;
  }
  if (req.has_header("Range") && req.path.rfind("/api/", 0) == 0) {
    send_error(res, 416, "nagłówek Range nie jest obsługiwany przez API");
    return true;
  }
  if (req.has_header("Transfer-Encoding")) {
    send_error(res, 411, "treść żądania musi mieć nagłówek Content-Length (przesyłanie w częściach nie jest obsługiwane)");
    return true;
  }
  uint64_t length = 0;
  if (req.has_header("Content-Length")) {
    const std::string value = req.get_header_value("Content-Length");
    if (req.get_header_value_count("Content-Length") != 1 || value.empty() || value.size() > 19 ||
        value.find_first_not_of("0123456789") != std::string::npos) {
      send_error(res, 400, "nieprawidłowy nagłówek Content-Length");
      return true;
    }
    length = std::stoull(value);
    if (length > max_body_bytes) {
      send_error(res, 413, "treść żądania jest za duża (najwyżej " + std::to_string(max_body_bytes / (1024 * 1024)) + " MB)");
      return true;
    }
  } else if (m == "POST" || m == "PUT") {  // httplib would read the body until the connection closes
    send_error(res, 411, "treść żądania musi mieć nagłówek Content-Length");
    return true;
  }
  // multipart bodies are not just read but parsed by httplib (files, boundaries) before any
  // handler: never let them in
  std::string type = req.get_header_value("Content-Type");
  for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (type.find("multipart/") != std::string::npos) {
    send_error(res, 415, "oczekiwano treści typu application/json");
    return true;
  }
  return false;
}

// A body only as JSON (a form, multipart or text/plain POST is what another web site could send
// cross-origin without a CORS preflight), and /api/setup always as JSON. Checked after the body
// was read (its length was bounded by refuse()); with one request per connection nothing that
// body contains is ever parsed as a request.
bool wrong_body_type(const httplib::Request& req) {
  const uint64_t length = req.get_header_value_u64("Content-Length");
  return (length > 0 || req.path == "/api/setup") && !is_json_type(req.get_header_value("Content-Type"));
}

void install_request_guard(httplib::Server& server, size_t max_body_bytes) {
  // One request per connection. With a keep-alive count of 1, httplib serves exactly one request,
  // answers it with "Connection: close" and closes the socket, so nothing a client sends after
  // the first request (a body httplib did not read, whatever the reason: a refusal below, a
  // multipart or encoding error inside httplib, HEAD, Range...) is ever parsed as a request. A
  // panel needs little else: one long live-feed connection and occasional fetches (each one a
  // new connection, well within httplib's listen backlog of 5).
  server.set_keep_alive_max_count(1);
  server.set_payload_max_length(max_body_bytes);  // a second line of defence: checked before routing below
  server.set_error_handler([max_body_bytes](const httplib::Request& req, httplib::Response& res) {
    if (res.has_header("Content-Type")) return httplib::Server::HandlerResponse::Unhandled;  // already answered
    if (res.status == 413 && !is_json_type(req.get_header_value("Content-Type"))) {
      // httplib's own limit for a form (urlencoded) body, 8 KB, far below ours: the type is wrong
      send_error(res, 413, "oczekiwano treści typu application/json");
    } else if (res.status == 413) {
      send_error(res, 413, "treść żądania jest za duża (najwyżej " + std::to_string(max_body_bytes / (1024 * 1024)) + " MB)");
    } else if (res.status == 400) {
      send_error(res, 400, "nieprawidłowe żądanie");
    } else {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    return httplib::Server::HandlerResponse::Handled;
  });
  // Runs on the headers, before any body is read: what cannot be read safely (refuse). The
  // connection closes after every answer (one request per connection), so an unread body is never
  // parsed; bounded bodies are read first and answered by guard().
  server.set_pre_routing_handler([max_body_bytes](const httplib::Request& req, httplib::Response& res) {
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Cache-Control", "no-store");
    return refuse(req, res, max_body_bytes) ? httplib::Server::HandlerResponse::Handled : httplib::Server::HandlerResponse::Unhandled;
  });
}

httplib::Server::Handler guard(Access access, std::function<bool(const httplib::Request&)> authorized, httplib::Server::Handler handler) {
  return [access, authorized = std::move(authorized), handler = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
    if (wrong_body_type(req)) return send_error(res, 415, "oczekiwano treści typu application/json");
    if (access == Access::Session && !authorized(req)) return send_error(res, 401, "wymagane logowanie");
    handler(req, res);
  };
}

void wait_listening(const httplib::Server& server, const std::atomic<bool>& listen_returned) {
  while (!server.is_running() && !listen_returned) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace bam::http
