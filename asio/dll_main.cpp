// SPDX-License-Identifier: GPL-3.0-or-later
// COM entry points and self-registration of the BounceAES Master ASIO driver.
#include <windows.h>
#include <objbase.h>
#include <olectl.h>

#include <new>
#include <string>

#include "asio_driver.hpp"

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_locks{0};
constexpr const wchar_t* kAsioKey = L"SOFTWARE\\ASIO\\BounceAES Master";
constexpr const wchar_t* kDescription = L"BounceAES Master";

class ClassFactory final : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // static instance
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* driver = new (std::nothrow) bam::BounceAsio();
    if (!driver) return E_OUTOFMEMORY;
    const HRESULT hr = driver->QueryInterface(riid, ppv);
    driver->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    lock ? ++g_locks : --g_locks;
    return S_OK;
  }
};

ClassFactory g_factory;

bool set_value(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  HKEY h;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
  const LONG rc = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(h);
  return rc == ERROR_SUCCESS;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (!IsEqualCLSID(clsid, bam::kBounceAsioClsid)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
  return (bam::g_object_count.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return SELFREG_E_CLASS;
  const std::wstring clsid_key = std::wstring(L"CLSID\\") + bam::kBounceAsioClsidString;
  bool ok = set_value(HKEY_CLASSES_ROOT, clsid_key, nullptr, L"BounceAES Master ASIO Driver");
  ok &= set_value(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", nullptr, path);
  ok &= set_value(HKEY_CLASSES_ROOT, clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
  ok &= set_value(HKEY_LOCAL_MACHINE, kAsioKey, L"CLSID", bam::kBounceAsioClsidString);
  ok &= set_value(HKEY_LOCAL_MACHINE, kAsioKey, L"Description", kDescription);
  return ok ? S_OK : SELFREG_E_CLASS;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
  const std::wstring clsid_key = std::wstring(L"CLSID\\") + bam::kBounceAsioClsidString;
  RegDeleteTreeW(HKEY_CLASSES_ROOT, clsid_key.c_str());
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAsioKey);
  return S_OK;
}
