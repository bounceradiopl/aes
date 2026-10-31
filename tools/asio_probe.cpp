// SPDX-License-Identifier: GPL-3.0-or-later
// asio-probe: does what an ASIO host (e.g. mAirList) does when it lists and opens drivers.
// Lists HKLM\SOFTWARE\ASIO as seen by this process (64-bit build: the 64-bit registry, 32-bit
// build: WOW6432Node), then loads "BounceAES Master" through COM and asks it for its name and
// channels. Build it in both bitnesses to check what 64-bit and 32-bit hosts see.
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

namespace {

constexpr const wchar_t* kDriverName = L"BounceAES Master";

std::string narrow(const wchar_t* s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

bool read_clsid(HKEY asio, const wchar_t* name, std::wstring* clsid) {
  wchar_t buf[64] = {};
  DWORD size = sizeof(buf);
  if (RegGetValueW(asio, name, L"CLSID", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
  *clsid = buf;
  return true;
}

}  // namespace

int wmain() {
  SetConsoleOutputCP(CP_UTF8);
  const int bits = static_cast<int>(sizeof(void*) * 8);
  std::printf("asio-probe %d-bit: sterowniki ASIO widoczne dla programów %d-bitowych\n", bits, bits);

  HKEY asio = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &asio) != ERROR_SUCCESS) {
    std::printf("  brak klucza HKLM\\SOFTWARE\\ASIO: żaden sterownik ASIO nie jest zarejestrowany\n");
    return 1;
  }
  bool found = false;
  std::wstring clsid_text;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD len = 256;
    if (RegEnumKeyExW(asio, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    std::wstring clsid;
    const bool has = read_clsid(asio, name, &clsid);
    std::printf("  - %s %s\n", narrow(name).c_str(), has ? narrow(clsid.c_str()).c_str() : "(brak CLSID)");
    if (_wcsicmp(name, kDriverName) == 0 && has) {
      found = true;
      clsid_text = clsid;
    }
  }
  RegCloseKey(asio);
  if (!found) {
    std::printf("BŁĄD: brak sterownika \"BounceAES Master\" na liście %d-bitowej\n", bits);
    return 1;
  }

  CLSID clsid{};
  if (CLSIDFromString(clsid_text.c_str(), &clsid) != NOERROR) {
    std::printf("BŁĄD: niepoprawny CLSID %s\n", narrow(clsid_text.c_str()).c_str());
    return 1;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IASIO* driver = nullptr;
  // ASIO convention: the interface id is the driver's CLSID
  const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&driver));
  if (FAILED(hr) || !driver) {
    std::printf("BŁĄD: nie można załadować sterownika (CoCreateInstance 0x%08lX); sprawdź rejestrację DLL\n",
                static_cast<unsigned long>(hr));
    CoUninitialize();
    return 1;
  }
  int rc = 0;
  char text[128] = {};
  driver->getDriverName(text);
  std::printf("  załadowano: \"%s\", wersja %ld\n", text, driver->getDriverVersion());
  if (!driver->init(GetDesktopWindow())) {
    char error[128] = {};
    driver->getErrorMessage(error);
    std::printf("BŁĄD: init() się nie powiódł: %s\n", error);
    rc = 1;
  } else {
    long inputs = 0, outputs = 0;
    driver->getChannels(&inputs, &outputs);
    const bool rate = driver->canSampleRate(48000.0) == ASE_OK;
    std::printf("  init OK: %ld wejść, %ld wyjść, 48 kHz %s\n", inputs, outputs, rate ? "tak" : "NIE");
    if (inputs < 2 || outputs < 2 || !rate) rc = 1;
  }
  driver->Release();
  CoUninitialize();
  std::printf("%s\n", rc == 0 ? "OK" : "BŁĄD");
  return rc;
}
