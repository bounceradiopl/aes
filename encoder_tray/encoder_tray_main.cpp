// SPDX-License-Identifier: GPL-3.0-or-later
// BounceEncoderTray: Bounce Encoder status icon in the notification area. Polls the service's
// public /api/health endpoint and opens the web panel as an app window (Microsoft Edge --app),
// or in the default browser. Modelled on tray/tray_main.cpp (BounceAESTray), which stays as it is:
// that one also runs the BounceAES audio bridge and reports ASIO and the PTP master role, none of
// which the encoder has.
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "bam/encoder/config.hpp"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kPromoteTimer = 2;  // Explorer registers the icon settings shortly after NIM_ADD
constexpr UINT kPollMs = 2000;
constexpr const wchar_t* kTitle = L"Bounce Encoder";
enum Command : UINT { kOpenPanel = 100, kOpenStats, kExit };
// Ok: every enabled stream sends and every enabled input has a signal; Warning: something does
// not; Down: the service does not answer.
enum class Health { Ok, Warning, Down };

HINSTANCE g_instance = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbar_created = 0;
HICON g_icons[3] = {};
Health g_health = Health::Down;
bool g_first_poll = true;

std::wstring program_data() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
  return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
}

// Where the panel listens: the service writes %ProgramData%\Bounce Encoder\panel.json at start
// ({"bind": "...", "port": n}; config.json itself is for administrators and the service only).
// A specific bind address is used as it is; 0.0.0.0, a missing file or anything unexpected means
// 127.0.0.1:8070.
struct PanelAddress {
  std::wstring host = L"127.0.0.1";
  uint16_t port = bam::encoder::kDefaultEncoderHttpPort;
};

PanelAddress panel_address() {
  PanelAddress a;
  std::ifstream f(std::filesystem::path(program_data() + L"\\Bounce Encoder\\panel.json"), std::ios::binary);
  if (!f) return a;
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    const auto j = nlohmann::json::parse(ss.str());
    const int port = j.value("port", 0);
    if (port >= 1 && port <= 65535) a.port = static_cast<uint16_t>(port);
    const std::string bind = j.value("bind", "");
    const bool ipv4_text = !bind.empty() && bind.size() <= 15 && bind.find_first_not_of("0123456789.") == std::string::npos;
    if (ipv4_text && bind != "0.0.0.0") a.host.assign(bind.begin(), bind.end());
  } catch (...) {
  }
  return a;
}

std::wstring panel_url(const wchar_t* fragment = L"") {
  const PanelAddress a = panel_address();
  return L"http://" + a.host + L":" + std::to_wstring(a.port) + L"/" + fragment;
}

// Logo with a status badge (resources 2-4), at the DPI-correct notification-area size.
HICON load_status_icon(int resource) {
  HICON icon = nullptr;
  if (FAILED(LoadIconMetric(g_instance, MAKEINTRESOURCEW(resource), LIM_SMALL, &icon))) {
    icon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  }
  return icon;
}

bool http_health(std::string* body) {
  bool ok = false;
  HINTERNET session =
      WinHttpOpen(L"BounceEncoderTray/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 1000, 1000, 1000, 1500);
  const PanelAddress a = panel_address();
  HINTERNET conn = WinHttpConnect(session, a.host.c_str(), a.port, 0);
  HINTERNET req =
      conn ? WinHttpOpenRequest(conn, L"GET", L"/api/health", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
      std::string chunk(avail, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
      body->append(chunk.data(), read);
    }
    ok = status == 200;
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}

void notify(const std::wstring& text, DWORD flags) {
  NOTIFYICONDATAW n = g_nid;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = flags;
  wcsncpy_s(n.szInfoTitle, kTitle, _TRUNCATE);
  wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

void poll() {
  std::string body;
  Health health = Health::Down;
  std::wstring tip = L"Bounce Encoder — usługa nie działa";
  if (http_health(&body)) {
    try {
      const auto j = nlohmann::json::parse(body);
      const int streams = j.value("streams_enabled", 0), streaming = j.value("streams_streaming", 0);
      const int inputs = j.value("inputs_enabled", 0), signal = j.value("inputs_signal", 0);
      const std::string ptp = j.value("ptp", "freerun");
      const wchar_t* ptp_text = ptp == "locked"      ? L"zsynchronizowany"
                                : ptp == "holdover"  ? L"holdover (brak mastera)"
                                : ptp == "listening" ? L"synchronizacja…"
                                : ptp == "disabled"  ? L"wyłączony (zegar lokalny)"
                                                     : L"brak mastera (zegar lokalny)";
      tip = std::wstring(L"Bounce Encoder\nStrumienie: ") + std::to_wstring(streaming) + L"/" + std::to_wstring(streams) +
            L" nadaje\nWejścia: " + std::to_wstring(signal) + L"/" + std::to_wstring(inputs) + L" z sygnałem\nPTP: " + ptp_text;
      health = streaming == streams && signal == inputs ? Health::Ok : Health::Warning;
    } catch (...) {
      health = Health::Warning;
    }
  }
  if (!g_first_poll && health != g_health) {
    if (health == Health::Down) notify(L"Usługa przestała odpowiadać", NIIF_ERROR);
    else if (health == Health::Warning) notify(L"Nie wszystkie strumienie nadają lub brak sygnału na wejściu", NIIF_WARNING);
    else notify(L"Wszystkie strumienie nadają", NIIF_INFO);
  }
  g_first_poll = false;
  g_health = health;
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  g_nid.hIcon = g_icons[static_cast<int>(health)];
  wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void open_panel(const wchar_t* fragment) {
  const std::wstring url = panel_url(fragment);
  const std::wstring args = L"--app=" + url;
  const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"msedge", args.c_str(), nullptr, SW_SHOWNORMAL));
  if (rc <= 32) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void add_icon(HWND hwnd) {
  g_nid = {};
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = hwnd;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_nid.uCallbackMessage = WM_TRAY;
  g_nid.hIcon = g_icons[static_cast<int>(g_health)];
  wcsncpy_s(g_nid.szTip, kTitle, _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &g_nid);
  g_nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

// Windows 11 hides new notification icons behind the ^ overflow. On first run, mark ours as
// "always shown" (HKCU\Control Panel\NotifyIconSettings\<id>\IsPromoted). A choice the user
// made later in Settings is respected: the value is only written when it does not exist yet.
void promote_icon() {
  wchar_t self[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
  const std::wstring exe_name = std::filesystem::path(self).filename().wstring();
  HKEY root;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
  wchar_t sub[256];
  for (DWORD i = 0;; ++i) {
    DWORD len = 256;
    if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY key;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS) continue;
    wchar_t path[1024];
    DWORD size = sizeof(path), type = 0;
    if (RegQueryValueExW(key, L"ExecutablePath", nullptr, &type, reinterpret_cast<BYTE*>(path), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
      const std::wstring p(path);
      const bool ours = p.size() >= exe_name.size() && _wcsicmp(p.c_str() + p.size() - exe_name.size(), exe_name.c_str()) == 0;
      if (ours && RegQueryValueExW(key, L"IsPromoted", nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        const DWORD one = 1;
        RegSetValueExW(key, L"IsPromoted", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
      }
    }
    RegCloseKey(key);
  }
  RegCloseKey(root);
}

void show_menu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpenPanel, L"Otwórz panel");
  AppendMenuW(menu, MF_STRING, kOpenStats, L"Statystyki");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Zakończ (usługa i strumienie działają dalej)");
  SetMenuDefaultItem(menu, kOpenPanel, FALSE);
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbar_created && g_taskbar_created != 0) {  // Explorer restarted
    add_icon(hwnd);
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      add_icon(hwnd);
      SetTimer(hwnd, kPollTimer, kPollMs, nullptr);
      SetTimer(hwnd, kPromoteTimer, 5000, nullptr);
      poll();
      return 0;
    case WM_TIMER:
      if (wp == kPromoteTimer) {
        KillTimer(hwnd, kPromoteTimer);
        promote_icon();
      } else {
        poll();
      }
      return 0;
    case WM_TRAY:
      switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
          open_panel(L"");
          break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
          show_menu(hwnd);
          break;
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kOpenPanel:
          open_panel(L"");
          break;
        case kOpenStats:
          open_panel(L"#stats");
          break;
        case kExit:
          DestroyWindow(hwnd);
          break;
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kPollTimer);
      Shell_NotifyIconW(NIM_DELETE, &g_nid);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int) {
  g_instance = instance;
  // Shortcuts pass --panel; the logon autostart entry does not, so logging in opens no window.
  const bool want_panel = cmdline && wcsstr(cmdline, L"--panel") != nullptr;
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BounceEncoderTray");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Already running (e.g. started again from a shortcut): just open the panel.
    open_panel(L"");
    return 0;
  }
  if (want_panel) open_panel(L"");
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  g_icons[static_cast<int>(Health::Ok)] = load_status_icon(2);
  g_icons[static_cast<int>(Health::Warning)] = load_status_icon(3);
  g_icons[static_cast<int>(Health::Down)] = load_status_icon(4);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"BounceEncoderTray";
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);
  CreateWindowExW(0, wc.lpszClassName, kTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (HICON i : g_icons) DestroyIcon(i);
  if (single) CloseHandle(single);
  return 0;
}
