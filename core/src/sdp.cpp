// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/sdp.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

#include "bam/ipv4.hpp"

namespace bam {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(sep, start);
    if (end == std::string_view::npos) end = s.size();
    if (end > start) out.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

template <typename T>
bool to_number(std::string_view s, T& out) {
  s = trim(s);
  auto res = std::from_chars(s.data(), s.data() + s.size(), out);
  return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

bool to_double(std::string_view s, double& out) {
  std::string tmp(trim(s));
  if (tmp.empty()) return false;
  char* end = nullptr;
  out = std::strtod(tmp.c_str(), &end);
  return end == tmp.c_str() + tmp.size();
}

// "IN IP4 239.1.14.100/15[/n]"
bool parse_connection(std::string_view v, std::string& addr, int& ttl) {
  auto parts = split(trim(v), ' ');
  if (parts.size() < 3 || parts[0] != "IN" || parts[1] != "IP4") return false;
  auto pieces = split(parts[2], '/');
  if (pieces.empty()) return false;
  addr = std::string(pieces[0]);
  ttl = 0;
  if (pieces.size() >= 2 && !to_number(pieces[1], ttl)) return false;
  return true;
}

struct MediaState {
  bool seen = false;
  std::string conn_addr;
  int conn_ttl = 0;
  std::string rtpmap;
  std::optional<double> ptime;
  std::optional<uint32_t> framecount;
  std::string source_filter;
  std::string refclk;
  std::optional<uint32_t> mediaclk;
};

void apply_attribute(std::string_view attr, MediaState& m, std::string& session_refclk,
                     std::optional<uint32_t>& session_mediaclk) {
  auto colon = attr.find(':');
  std::string_view name = attr.substr(0, colon);
  std::string_view value = colon == std::string_view::npos ? std::string_view{} : trim(attr.substr(colon + 1));
  if (name == "rtpmap") {
    m.rtpmap = std::string(value);
  } else if (name == "ptime") {
    double p = 0;
    if (to_double(value, p) && p > 0) m.ptime = p;
  } else if (name == "framecount") {
    uint32_t f = 0;
    if (to_number(value, f) && f > 0) m.framecount = f;
  } else if (name == "source-filter") {
    // "incl IN IP4 <dest> <src> [<src>...]"
    auto parts = split(value, ' ');
    if (parts.size() >= 5 && parts[0] == "incl") m.source_filter = std::string(parts[4]);
  } else if (name == "ts-refclk") {
    (m.seen ? m.refclk : session_refclk) = std::string(value);
  } else if (name == "mediaclk") {
    uint32_t off = 0;
    if (value.substr(0, 7) == "direct=" && to_number(value.substr(7), off)) {
      (m.seen ? m.mediaclk : session_mediaclk) = off;
    }
  }
}

}  // namespace

SdpParseResult parse_sdp(std::string_view text) {
  SdpParseResult result;
  SdpStream s;
  std::string session_conn;
  int session_ttl = 0;
  std::string session_refclk;
  std::optional<uint32_t> session_mediaclk;
  MediaState media;
  bool media_done = false;
  std::vector<std::string_view> fmt_payloads;

  for (auto raw : split(text, '\n')) {
    auto line = trim(raw);
    if (line.size() < 2 || line[1] != '=') continue;
    char type = line[0];
    std::string_view value = line.substr(2);
    if (type == 'm') {
      if (media.seen) {
        media_done = true;  // only the first media section is used
        continue;
      }
      auto parts = split(value, ' ');
      if (parts.size() < 4 || parts[0] != "audio") continue;
      media.seen = true;
      if (!to_number(parts[1], s.port)) {
        result.error = "nieprawidłowy port w linii m=";
        return result;
      }
      if (!to_number(parts[3], s.payload_type)) {
        result.error = "nieprawidłowy typ ładunku w linii m=";
        return result;
      }
      continue;
    }
    if (media_done) continue;
    switch (type) {
      case 'o': {
        auto parts = split(value, ' ');
        if (parts.size() >= 6) s.origin_address = std::string(parts[5]);
        break;
      }
      case 's':
        if (!media.seen) s.session_name = std::string(trim(value));
        break;
      case 'c':
        if (media.seen) {
          parse_connection(value, media.conn_addr, media.conn_ttl);
        } else {
          parse_connection(value, session_conn, session_ttl);
        }
        break;
      case 'a':
        apply_attribute(value, media, session_refclk, session_mediaclk);
        break;
      default:
        break;
    }
  }

  if (!media.seen) {
    result.error = "brak sekcji m=audio w SDP";
    return result;
  }

  s.multicast_address = media.conn_addr.empty() ? session_conn : media.conn_addr;
  s.ttl = media.conn_addr.empty() ? session_ttl : media.conn_ttl;
  auto ip = parse_ipv4(s.multicast_address);
  if (!ip) {
    result.error = "brak lub nieprawidłowy adres w linii c=";
    return result;
  }
  if (!is_multicast(*ip)) {
    result.error = "adres " + s.multicast_address + " nie jest adresem multicast";
    return result;
  }

  // rtpmap: "<pt> <enc>/<rate>[/<channels>]"
  if (media.rtpmap.empty()) {
    result.error = "brak a=rtpmap dla typu ładunku " + std::to_string(s.payload_type);
    return result;
  }
  {
    auto sp = media.rtpmap.find(' ');
    std::string_view desc = sp == std::string::npos ? std::string_view{} : std::string_view(media.rtpmap).substr(sp + 1);
    auto parts = split(trim(desc), '/');
    if (parts.size() < 2) {
      result.error = "nieprawidłowe a=rtpmap: " + media.rtpmap;
      return result;
    }
    if (parts[0] == "L24") {
      s.encoding = Encoding::L24;
    } else if (parts[0] == "L16") {
      s.encoding = Encoding::L16;
    } else {
      result.error = "nieobsługiwane kodowanie: " + std::string(parts[0]) + " (obsługiwane L16 i L24)";
      return result;
    }
    if (!to_number(parts[1], s.sample_rate)) {
      result.error = "nieprawidłowa częstotliwość w a=rtpmap";
      return result;
    }
    s.channels = 1;
    if (parts.size() >= 3 && !to_number(parts[2], s.channels)) {
      result.error = "nieprawidłowa liczba kanałów w a=rtpmap";
      return result;
    }
  }
  if (s.sample_rate != 48000) {
    result.error = "nieobsługiwana częstotliwość: " + std::to_string(s.sample_rate) + " Hz (wymagane 48000 Hz)";
    return result;
  }
  if (s.channels < 1 || s.channels > 2) {
    result.error = "nieobsługiwana liczba kanałów: " + std::to_string(s.channels) + " (obsługiwane 1 lub 2)";
    return result;
  }

  s.framecount = media.framecount;
  if (media.ptime) {
    s.ptime_ms = *media.ptime;
  } else if (media.framecount) {
    s.ptime_ms = *media.framecount * 1000.0 / s.sample_rate;
  } else {
    s.ptime_ms = 1.0;
  }
  s.frames_per_packet = static_cast<uint32_t>(std::lround(s.ptime_ms * s.sample_rate / 1000.0));
  if (s.frames_per_packet == 0) {
    result.error = "nieprawidłowy ptime";
    return result;
  }

  s.source_filter = media.source_filter;

  // "ptp=IEEE1588-2008:30-D6-59-FF-FE-01-C1-19:0"
  std::string refclk = media.refclk.empty() ? session_refclk : media.refclk;
  if (refclk.rfind("ptp=", 0) == 0) {
    auto parts = split(std::string_view(refclk).substr(4), ':');
    if (parts.size() >= 2 && parts[1] != "traceable") s.ptp_gmid = std::string(parts[1]);
    if (parts.size() >= 3) to_number(parts[2], s.ptp_domain);
  }
  s.mediaclk_offset = media.mediaclk.value_or(session_mediaclk.value_or(0));

  result.stream = std::move(s);
  return result;
}

std::string generate_sdp(const SdpStream& s, uint64_t session_id, uint64_t version) {
  char ptime[32];
  if (std::fabs(s.ptime_ms - std::round(s.ptime_ms)) < 1e-9) {
    std::snprintf(ptime, sizeof(ptime), "%.0f", s.ptime_ms);
  } else {
    std::snprintf(ptime, sizeof(ptime), "%.8f", s.ptime_ms);
  }
  std::string refclk = "a=ts-refclk:ptp=IEEE1588-2008:" + (s.ptp_gmid.empty() ? std::string("traceable") : s.ptp_gmid + ":" + std::to_string(s.ptp_domain));
  std::string conn = "c=IN IP4 " + s.multicast_address + "/" + std::to_string(s.ttl);
  std::ostringstream o;
  o << "v=0\r\n"
    << "o=- " << session_id << " " << version << " IN IP4 " << s.origin_address << "\r\n"
    << "s=" << s.session_name << "\r\n"
    << conn << "\r\n"
    << "t=0 0\r\n"
    << "a=clock-domain:PTPv2 " << s.ptp_domain << "\r\n"
    << refclk << "\r\n"
    << "a=mediaclk:direct=" << s.mediaclk_offset << "\r\n"
    << "m=audio " << s.port << " RTP/AVP " << s.payload_type << "\r\n"
    << conn << "\r\n"
    << "a=rtpmap:" << s.payload_type << " " << encoding_name(s.encoding) << "/" << s.sample_rate << "/" << s.channels << "\r\n"
    << "a=source-filter: incl IN IP4 " << s.multicast_address << " " << s.origin_address << "\r\n"
    << "a=clock-domain:PTPv2 " << s.ptp_domain << "\r\n"
    << "a=sync-time:0\r\n"
    << "a=framecount:" << s.frames_per_packet << "\r\n"
    << "a=ptime:" << ptime << "\r\n"
    << refclk << "\r\n"
    << "a=mediaclk:direct=" << s.mediaclk_offset << "\r\n"
    << "a=recvonly\r\n";
  return o.str();
}

}  // namespace bam
