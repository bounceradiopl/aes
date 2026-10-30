// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAESTray: status icon in the notification area. Polls the service's public /api/health
// endpoint and opens the web panel as an app window (Microsoft Edge --app), or in the default browser.
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "bam/constants.hpp"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kPollMs = 2000;
enum Command : UINT { kOpenPanel = 100, kOpenStats, kExit };
enum class Health { Ok, Warning, Down };

HINSTANCE g_instance = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbar_created = 0;
HICON g_icons[3] = {};
Health g_health = Health::Down;
std::string g_ptp;
bool g_first_poll = true;

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
      const wchar_t* ptp_text = ptp == "locked"     ? L"zsynchronizowany"
                                : ptp == "holdover" ? L"holdover (brak mastera)"
                                : ptp == "listening" ? L"synchronizacja…"
                                                     : L"brak mastera PTP";
      tip = std::wstring(L"BounceAES Master\nPTP: ") + ptp_text + L"\nASIO: " + (asio ? L"aplikacja połączona" : L"brak aplikacji");
      health = ptp == "locked" ? Health::Ok : Health::Warning;
    } catch (...) {
      health = Health::Warning;
    }
  }
  if (!g_first_poll && ptp != g_ptp) {
    if (ptp == "down") notify(L"BounceAES Master", L"Usługa przestała odpowiadać", NIIF_ERROR);
    else if (ptp == "holdover") notify(L"BounceAES Master", L"Utracono synchronizację PTP (holdover)", NIIF_WARNING);
    else if (ptp == "locked" && g_ptp != "") notify(L"BounceAES Master", L"PTP zsynchronizowany", NIIF_INFO);
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

void show_menu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpenPanel, L"Otwórz panel");
  AppendMenuW(menu, MF_STRING, kOpenStats, L"Statystyki");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Zakończ (usługa działa dalej)");
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
      poll();
      return 0;
    case WM_TIMER:
      poll();
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BounceAESMasterTray");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  g_instance = instance;
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
