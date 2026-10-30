// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAESTray: status icon in the notification area. Polls the service's public /api/health
// endpoint and opens the web panel as an app window (Microsoft Edge --app), or in the default browser.
// It also runs BounceAESAudioBridge.exe (Windows audio in this user session) and restarts it if it stops.
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <winhttp.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "bam/constants.hpp"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kPromoteTimer = 2;  // Explorer registers the icon settings shortly after NIM_ADD
constexpr UINT kPollMs = 2000;
constexpr UINT_PTR kBridgeTimer = 3;
enum Command : UINT { kOpenPanel = 100, kOpenStats, kExit };
enum class Health { Ok, Warning, Down };

HINSTANCE g_instance = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbar_created = 0;
HICON g_icons[3] = {};
Health g_health = Health::Down;
std::string g_ptp;
bool g_first_poll = true;

// ---- audio bridge supervision ----
HANDLE g_bridge = nullptr;
ULONGLONG g_bridge_start_at = 0;  // GetTickCount64 time of the next start attempt
std::deque<ULONGLONG> g_bridge_failures;

std::wstring program_data() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
  return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
}

uint16_t panel_port() {
  std::ifstream f(std::filesystem::path(program_data() + L"\\BounceAES Master\\config.json"), std::ios::binary);
  if (!f) return bam::kDefaultHttpPort;
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str()).value("http_port", bam::kDefaultHttpPort);
  } catch (...) {
    return bam::kDefaultHttpPort;
  }
}

std::wstring panel_url(const wchar_t* fragment = L"") {
  return L"http://127.0.0.1:" + std::to_wstring(panel_port()) + L"/" + fragment;
}

// Bounce AES logo with a status badge (resources 2-4), at the DPI-correct notification-area size.
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
  HINTERNET session = WinHttpOpen(L"BounceAESTray/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 1000, 1000, 1000, 1500);
  HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", panel_port(), 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", L"/api/health", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
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

void notify(const wchar_t* title, const std::wstring& text, DWORD flags) {
  NOTIFYICONDATAW n = g_nid;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = flags;
  wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

void poll() {
  std::string body;
  Health health = Health::Down;
  std::wstring tip = L"BounceAES Master — usługa nie działa";
  std::string ptp = "down";
  if (http_health(&body)) {
    try {
      const auto j = nlohmann::json::parse(body);
      ptp = j.value("ptp", "freerun");
      const bool asio = j.value("asio_connected", false);
      const wchar_t* ptp_text = ptp == "master"     ? L"master (ten komputer nadaje zegar)"
                                : ptp == "locked"   ? L"zsynchronizowany"
                                : ptp == "holdover" ? L"holdover (brak mastera)"
                                : ptp == "listening" ? L"synchronizacja…"
                                                     : L"brak mastera PTP";
      tip = std::wstring(L"BounceAES Master\nPTP: ") + ptp_text + L"\nASIO: " + (asio ? L"aplikacja połączona" : L"brak aplikacji");
      health = ptp == "locked" || ptp == "master" ? Health::Ok : Health::Warning;
    } catch (...) {
      health = Health::Warning;
    }
  }
  if (!g_first_poll && ptp != g_ptp) {
    if (ptp == "down") notify(L"BounceAES Master", L"Usługa przestała odpowiadać", NIIF_ERROR);
    else if (ptp == "holdover") notify(L"BounceAES Master", L"Utracono synchronizację PTP (holdover)", NIIF_WARNING);
    else if (ptp == "locked" && g_ptp != "") notify(L"BounceAES Master", L"PTP zsynchronizowany", NIIF_INFO);
    else if (ptp == "master") notify(L"BounceAES Master", L"Ten komputer jest masterem PTP sieci", NIIF_INFO);
  }
  g_first_poll = false;
  g_ptp = ptp;
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
  wcsncpy_s(g_nid.szTip, L"BounceAES Master", _TRUNCATE);
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

std::wstring bridge_path() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path, n);
  return p.substr(0, p.find_last_of(L"\\/") + 1) + L"BounceAESAudioBridge.exe";
}

void start_bridge() {
  std::wstring cmd = L"\"" + bridge_path() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    g_bridge_start_at = GetTickCount64() + 30000;  // missing file or no rights: try again later
    return;
  }
  CloseHandle(pi.hThread);
  g_bridge = pi.hProcess;
}

// Restart after 2 s; after three failures within a minute wait 30 s.
void supervise_bridge() {
  const ULONGLONG now = GetTickCount64();
  if (g_bridge && WaitForSingleObject(g_bridge, 0) == WAIT_OBJECT_0) {
    CloseHandle(g_bridge);
    g_bridge = nullptr;
    g_bridge_failures.push_back(now);
    while (!g_bridge_failures.empty() && now - g_bridge_failures.front() > 60000) g_bridge_failures.pop_front();
    g_bridge_start_at = now + (g_bridge_failures.size() >= 3 ? 30000 : 2000);
  }
  if (!g_bridge && now >= g_bridge_start_at) start_bridge();
}

void stop_bridge() {
  if (!g_bridge) return;
  if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\BounceAESAudioBridge.quit")) {
    SetEvent(quit);
    CloseHandle(quit);
  }
  if (WaitForSingleObject(g_bridge, 3000) != WAIT_OBJECT_0) TerminateProcess(g_bridge, 0);
  CloseHandle(g_bridge);
  g_bridge = nullptr;
}

void show_menu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpenPanel, L"Otwórz panel");
  AppendMenuW(menu, MF_STRING, kOpenStats, L"Statystyki");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Zakończ (usługa działa dalej, audio Windows zatrzymane)");
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
      SetTimer(hwnd, kBridgeTimer, 1000, nullptr);
      start_bridge();
      poll();
      return 0;
    case WM_TIMER:
      if (wp == kPromoteTimer) {
        KillTimer(hwnd, kPromoteTimer);
        promote_icon();
      } else if (wp == kBridgeTimer) {
        supervise_bridge();
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
      KillTimer(hwnd, kBridgeTimer);
      stop_bridge();
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
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BounceAESMasterTray");
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
  wc.lpszClassName = L"BounceAESMasterTray";
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);
  CreateWindowExW(0, wc.lpszClassName, L"BounceAES Master", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (HICON i : g_icons) DestroyIcon(i);
  if (single) CloseHandle(single);
  return 0;
}
