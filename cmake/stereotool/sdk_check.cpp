// SPDX-License-Identifier: GPL-3.0-or-later
// Build-time check, compiled only with the pinned Stereo Tool SDK (-DBAM_WITH_STEREOTOOL=ON,
// cmake/stereotool.cmake): the binding's own declarations of the libStereoTool functions
// (encoder/src/stereotool_api.hpp) have exactly the types of the SDK's libStereoTool.h, and its
// hard-coded parameter indices are those of ParameterEnum.h. Nothing of this is linked anywhere.
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "stereotool_api.hpp"

#include <ParameterEnum.h>
#include <libStereoTool.h>

namespace st = bam::encoder::detail::st;

static_assert(std::is_same_v<st::GlobalSetBindIpFn, decltype(&::stereoTool_global_setBindIp)>);
static_assert(std::is_same_v<st::GetSoftwareVersionFn, decltype(&::stereoTool_GetSoftwareVersion)>);
static_assert(std::is_same_v<st::Create3Fn, decltype(&::stereoTool_Create3)>);
static_assert(std::is_same_v<st::DeleteFn, decltype(&::stereoTool_Delete)>);
static_assert(std::is_same_v<st::ProcessFn, decltype(&::stereoTool_Process)>);
static_assert(std::is_same_v<st::GetLatency2Fn, decltype(&::stereoTool_GetLatency2)>);
static_assert(std::is_same_v<st::LoadPresetFn, decltype(&::stereoTool_LoadPreset)>);
static_assert(std::is_same_v<st::SavePresetFn, decltype(&::stereoTool_SavePreset)>);
static_assert(std::is_same_v<st::SetStsValueFn, decltype(&::stereoTool_SetStsValue)>);
static_assert(std::is_same_v<st::CheckLicenseValidFn, decltype(&::stereoTool_CheckLicenseValid)>);
static_assert(std::is_same_v<st::GetUnlicensedUsedFeaturesFn, decltype(&::stereoTool_GetUnlicensedUsedFeatures)>);

static_assert(st::kParamWebserverEnable == PARAM_WebserverEnable, "PARAM_WebserverEnable moved in ParameterEnum.h");
static_assert(st::kParamWebserverPort == PARAM_WebserverPort, "PARAM_WebserverPort moved in ParameterEnum.h");
static_assert(st::kSaveAllSettings == ID_SAVE_ALLSETTINGS, "ID_SAVE_ALLSETTINGS moved in ParameterEnum.h");
