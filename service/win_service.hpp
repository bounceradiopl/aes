// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <functional>

namespace bam {

inline constexpr const wchar_t* kServiceName = L"BounceAESMaster";
inline constexpr const wchar_t* kEventSource = L"BounceAES Master";

// Runs `body` as the Windows service main of the service `service_name` (BounceAES Master by
// default). `body` receives a stop flag set by the SCM.
// Returns false when the process was not started by the service control manager.
bool run_as_windows_service(std::function<int(std::atomic<bool>& stop)> body, const wchar_t* service_name = kServiceName);

// Forwards warnings and errors to the Windows Event Log (source "BounceAES Master" by default).
void install_event_log_sink(const wchar_t* source = kEventSource);

}  // namespace bam
