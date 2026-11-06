// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: the libStereoTool functions and constants the binding uses, declared here because
// the Thimeo headers are not part of this repository. Builds with the SDK
// (-DBAM_WITH_STEREOTOOL=ON) check every type and value below against libStereoTool.h and
// ParameterEnum.h 11.07 at compile time (cmake/stereotool/sdk_check.cpp).
#pragma once
#include <cstdint>

class gStereoTool;

namespace bam::encoder::detail::st {

using GlobalSetBindIpFn = void (*)(const char* web, const char* uecp);
using GetSoftwareVersionFn = int (*)();
using Create3Fn = gStereoTool* (*)(bool has_gui, const char* key, const char* st_name, const char* home_path,
                                   bool load_ini);
using DeleteFn = void (*)(gStereoTool*);
using ProcessFn = void (*)(gStereoTool*, float* samples, int32_t numsamples, int32_t channels, int32_t samplerate);
using GetLatency2Fn = int (*)(gStereoTool*, int32_t samplerate, bool feed_silence);
using LoadPresetFn = bool (*)(gStereoTool*, const char* filename, int loadsave_type);
using SavePresetFn = bool (*)(gStereoTool*, const char* filename, int loadsave_type);
using SetStsValueFn = bool (*)(gStereoTool*, int index, int subindex, const char* value);
using CheckLicenseValidFn = bool (*)(gStereoTool*);
using GetUnlicensedUsedFeaturesFn = bool (*)(gStereoTool*, char* text, int text_maxlen);

// enum ID of ParameterEnum.h 11.07 (positions; the enum grows between releases, so a new
// library version means checking these again: the SDK check does it at build time)
inline constexpr int kParamWebserverEnable = 524;  // PARAM_WebserverEnable, "Enable web interface" (0/1)
inline constexpr int kParamWebserverPort = 525;    // PARAM_WebserverPort, "Port"
inline constexpr int kSaveAllSettings = 20386;     // ID_SAVE_ALLSETTINGS: all but the configuration

}  // namespace bam::encoder::detail::st
