// SPDX-License-Identifier: GPL-3.0-or-later
// Request hardening shared by the BounceAES Master and Bounce Encoder panel servers (cpp-httplib):
//  - one request per connection, so nothing a client sends after the first request (a body
//    httplib did not read, whatever the reason) is ever parsed as another request;
//  - a request allowlist checked on the headers, before any body is read (refuse());
//  - after the body was read, the body type (JSON only) and the session (guard()).
// Every answer this file gives is JSON {"error": "<Polish message>"}.
#pragma once
#include <httplib.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace bam::http {

inline constexpr size_t kMaxBodyBytes = 1024 * 1024;  // larger request bodies get 413

void send_json(httplib::Response& res, const nlohmann::ordered_json& j, int status = 200);
void send_error(httplib::Response& res, int status, const std::string& message);

// The value of cookie `name` in the request's Cookie header ("" when missing).
std::string session_cookie(const httplib::Request& req, const std::string& name);

// The request comes from this computer (127.0.0.1 or ::1).
bool is_local_address(const httplib::Request& req);
// The Host header names this computer by a loopback literal: 127.0.0.1, localhost or [::1], with
// an optional port (case-insensitive). A page that rebinds its own domain to 127.0.0.1 (DNS
// rebinding) sends requests from 127.0.0.1, same-origin, but with its own name as the Host.
bool is_loopback_host(const httplib::Request& req);
// A request from a browser on this computer that opened the panel by a loopback address: the
// only place the first password may be set from.
inline bool is_local(const httplib::Request& req) { return is_local_address(req) && is_loopback_host(req); }

// The JSON media type, exactly: "application/json", optionally with a charset parameter.
bool is_json_type(const std::string& value);

// The request allowlist, on the headers only. true: refused (answered). See guard.cpp.
bool refuse(const httplib::Request& req, httplib::Response& res, size_t max_body_bytes = kMaxBodyBytes);

// A body that is not JSON (and /api/setup always needs a JSON body). Checked after the body was read.
bool wrong_body_type(const httplib::Request& req);

// The body size limit of one request, decided on its headers (method, path, cookie) before any
// body is read; at most the max_body_bytes given to install_request_guard.
using BodyLimit = std::function<size_t(const httplib::Request&)>;

// Sets up `server`: one request per connection, the body size limit, JSON answers for httplib's
// own refusals, and a pre-routing handler with the security headers and refuse(). With `limit`,
// each request gets the smaller of limit(req) and `max_body_bytes` (e.g. one upload route
// allowed more than the rest); without it, `max_body_bytes` for every request.
void install_request_guard(httplib::Server& server, size_t max_body_bytes = kMaxBodyBytes, BodyLimit limit = {});

enum class Access { Public, Session };

// Wraps a route handler: the body type (415) and, for Access::Session, `authorized` (401), both
// checked after the body was read; `handler` runs only when both pass.
httplib::Server::Handler guard(Access access, std::function<bool(const httplib::Request&)> authorized, httplib::Server::Handler handler);

// httplib's Server::stop() does nothing until the listen thread has set is_running(): a stop that
// comes first would leave the thread listening for ever and its join() hanging. Waits until the
// server listens or its listen thread has ended (`listen_returned`). Short: the thread only has
// to start.
void wait_listening(const httplib::Server& server, const std::atomic<bool>& listen_returned);

}  // namespace bam::http
