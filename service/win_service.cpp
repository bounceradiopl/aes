// SPDX-License-Identifier: GPL-3.0-or-later
#include "win_service.hpp"

#include <windows.h>

#include <string>

#include "bam/log.hpp"

namespace bam {

namespace {
SERVICE_STATUS_HANDLE g_handle = nullptr;
SERVICE_STATUS g_status{};
std::atomic<bool>* g_stop = nullptr;
std::function<int(std::atomic<bool>&)> g_body;

void report(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
  static DWORD checkpoint = 1;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = exit_code;
  g_status.dwWaitHint = wait_hint;
  g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
  SetServiceStatus(g_handle, &g_status);
}

DWORD WINAPI control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      report(SERVICE_STOP_PENDING, NO_ERROR, 10000);
      if (g_stop) *g_stop = true;
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
  if (!g_handle) return;
  report(SERVICE_START_PENDING, NO_ERROR, 10000);
  std::atomic<bool> stop{false};
  g_stop = &stop;
  report(SERVICE_RUNNING);
  const int rc = g_body(stop);
  g_stop = nullptr;
  report(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}
}  // namespace

bool run_as_windows_service(std::function<int(std::atomic<bool>& stop)> body) {
  g_body = std::move(body);
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), service_main}, {nullptr, nullptr}};
  if (StartServiceCtrlDispatcherW(table)) return true;
  return GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
}

void install_event_log_sink() {
  HANDLE source = RegisterEventSourceW(nullptr, L"BounceAES Master");
  if (!source) return;
  Logger::instance().add_sink([source](const LogEntry& e) {
    if (e.level == LogLevel::Info) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, e.message.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, e.message.c_str(), -1, w.data(), n);
    LPCWSTR strings[] = {w.c_str()};
    ReportEventW(source, e.level == LogLevel::Error ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE, 0, 1, nullptr, 1, 0, strings,
                 nullptr);
  });
}

}  // namespace bam
