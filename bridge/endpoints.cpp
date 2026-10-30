// SPDX-License-Identifier: GPL-3.0-or-later
#include "endpoints.hpp"

#include <windows.h>
// windows.h first
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>

#include "com_ptr.hpp"

namespace bam::bridge {

namespace {
std::string narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string device_id(IMMDevice* d) {
  LPWSTR id = nullptr;
  std::string out;
  if (SUCCEEDED(d->GetId(&id))) out = narrow(id);
  if (id) CoTaskMemFree(id);
  return out;
}

std::string friendly_name(IMMDevice* d) {
  ComPtr<IPropertyStore> props;
  if (FAILED(d->OpenPropertyStore(STGM_READ, props.put()))) return {};
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string out;
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out = narrow(v.pwszVal);
  PropVariantClear(&v);
  return out;
}

std::string default_id(IMMDeviceEnumerator* e, EDataFlow flow) {
  ComPtr<IMMDevice> d;
  if (FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, d.put()))) return {};
  return device_id(d.get());
}

std::string process_exe(uint32_t pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t path[MAX_PATH * 2];
  DWORD n = MAX_PATH * 2;
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, path, &n)) {
    std::wstring p(path, n);
    const auto slash = p.find_last_of(L"\\/");
    out = lower(narrow((slash == std::wstring::npos ? p : p.substr(slash + 1)).c_str()));
  }
  CloseHandle(h);
  return out;
}
}  // namespace

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(c < 0x80 ? std::tolower(c) : c); });
  return s;
}

std::vector<BridgeEndpoint> list_endpoints() {
  std::vector<BridgeEndpoint> out;
  ComPtr<IMMDeviceEnumerator> e;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put())))) return out;
  const std::string def_render = default_id(e.get(), eRender);
  const std::string def_capture = default_id(e.get(), eCapture);
  ComPtr<IMMDeviceCollection> list;
  if (FAILED(e->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, list.put()))) return out;
  UINT count = 0;
  list->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> d;
    if (FAILED(list->Item(i, d.put()))) continue;
    ComPtr<IMMEndpoint> ep;
    EDataFlow flow = eRender;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(ep.put())))) ep->GetDataFlow(&flow);
    BridgeEndpoint b;
    b.id = device_id(d.get());
    b.name = friendly_name(d.get());
    b.render = flow == eRender;
    b.default_device = b.id == (b.render ? def_render : def_capture);
    out.push_back(std::move(b));
  }
  return out;
}

std::vector<BridgeApp> list_apps() {
  std::map<std::string, BridgeApp> apps;
  ComPtr<IMMDeviceEnumerator> e;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put())))) return {};
  ComPtr<IMMDeviceCollection> list;
  if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.put()))) return {};
  UINT count = 0;
  list->GetCount(&count);
  const DWORD self = GetCurrentProcessId();
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> d;
    if (FAILED(list->Item(i, d.put()))) continue;
    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(mgr.put())))) continue;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(mgr->GetSessionEnumerator(sessions.put()))) continue;
    int n = 0;
    sessions->GetCount(&n);
    for (int k = 0; k < n; ++k) {
      ComPtr<IAudioSessionControl> ctl;
      if (FAILED(sessions->GetSession(k, ctl.put()))) continue;
      ComPtr<IAudioSessionControl2> ctl2;
      if (FAILED(ctl->QueryInterface(IID_PPV_ARGS(ctl2.put())))) continue;
      DWORD pid = 0;
      if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;  // 0: system sounds
      const std::string exe = process_exe(pid);
      if (exe.empty() || apps.count(exe)) continue;
      BridgeApp a;
      a.exe = exe;
      a.pid = pid;
      LPWSTR display = nullptr;
      if (SUCCEEDED(ctl->GetDisplayName(&display)) && display && *display && display[0] != L'@') a.name = narrow(display);
      if (display) CoTaskMemFree(display);
      if (a.name.empty()) {
        a.name = exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0 ? exe.substr(0, exe.size() - 4) : exe;
        if (!a.name.empty()) a.name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(a.name[0])));
      }
      apps[exe] = std::move(a);
    }
  }
  std::vector<BridgeApp> out;
  for (auto& [exe, a] : apps) out.push_back(std::move(a));
  return out;
}

uint32_t find_process(const std::string& exe) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  std::map<DWORD, DWORD> parent;  // pid -> parent pid, for processes with this executable
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (lower(narrow(pe.szExeFile)) == exe) parent[pe.th32ProcessID] = pe.th32ParentProcessID;
  CloseHandle(snap);
  // The root of the tree (its parent is a different program) also captures its child processes,
  // e.g. browser renderers.
  for (const auto& [pid, ppid] : parent)
    if (!parent.count(ppid)) return pid;
  return parent.empty() ? 0 : parent.begin()->first;
}

uint32_t windows_build() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  if (!fn || fn(&v) != 0) return 0;
  return v.dwBuildNumber;
}

}  // namespace bam::bridge
