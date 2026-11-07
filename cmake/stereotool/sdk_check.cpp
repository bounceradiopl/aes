// SPDX-License-Identifier: GPL-3.0-or-later
// Build-time check, compiled only with the pinned Stereo Tool SDK (-DBAM_WITH_STEREOTOOL=ON,
// cmake/stereotool.cmake): the binding's own declarations of the libStereoTool functions
// (encoder/src/stereotool_api.hpp) have exactly the types of the SDK's libStereoTool.h, and its
// hard-coded parameter indices are those of ParameterEnum.h. Nothing of this is linked anywhere.
#include <cstddef>
#include <cstdint>
#include <iterator>
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
static_assert(std::is_same_v<st::GetStsValueFn, decltype(&::stereoTool_GetStsValue)>);
static_assert(std::is_same_v<st::CheckLicenseValidFn, decltype(&::stereoTool_CheckLicenseValid)>);
static_assert(std::is_same_v<st::GetUnlicensedUsedFeaturesFn, decltype(&::stereoTool_GetUnlicensedUsedFeatures)>);

static_assert(st::kParamWebserverEnable == PARAM_WebserverEnable, "PARAM_WebserverEnable moved in ParameterEnum.h");
static_assert(st::kParamWebserverPort == PARAM_WebserverPort, "PARAM_WebserverPort moved in ParameterEnum.h");
static_assert(st::kSaveAllSettings == ID_SAVE_ALLSETTINGS, "ID_SAVE_ALLSETTINGS moved in ParameterEnum.h");

static_assert(st::kParamPasswordProtectionAny == PARAM_PASSWORD_PROTECTIONON_ANY, "PARAM_PASSWORD_PROTECTIONON_ANY moved");
static_assert(st::kParamPasswordProtection0 == PARAM_PASSWORD_PROTECTIONON_0, "PARAM_PASSWORD_PROTECTIONON_0 moved");
static_assert(st::kParamPassword0 == PARAM_PASSWORD_STRING_0, "PARAM_PASSWORD_STRING_0 moved");
static_assert(st::kParamJsonToken == PARAM_PASSWORD_JSON_TOKEN, "PARAM_PASSWORD_JSON_TOKEN moved");

// The network switches (stereotool_api.hpp kNetworkSwitches), each range by its first and last name:
// a range with a gap or a moved end fails here.
constexpr bool range(size_t i, int first, int last) {
  return st::kNetworkSwitches[i].first == first && st::kNetworkSwitches[i].first + st::kNetworkSwitches[i].count - 1 == last;
}
static_assert(std::size(st::kNetworkSwitches) == 14, "every range of kNetworkSwitches is checked below");
static_assert(range(0, PARAM_STREAMOUTPUT_Enabled, PARAM_STREAMOUTPUT_Enabled));
static_assert(range(1, PARAM_STREAMOUTPUT_Enabled_0, PARAM_STREAMOUTPUT_Enabled_6));
static_assert(range(2, PARAM_UECP_Enable, PARAM_UECP_Enable));
static_assert(range(3, PARAM_UECP_Enable_1, PARAM_UECP_Enable_9));
static_assert(range(4, PARAM_UMPX_Encoder_Enabled, PARAM_UMPX_Encoder_Enabled));
static_assert(range(5, PARAM_UMPX_Enabled_0, PARAM_UMPX_Enabled_99));
static_assert(range(6, PARAM_UMPX_Decoder_Enabled, PARAM_UMPX_Decoder_Enabled));
static_assert(range(7, PARAM_Kantar_on, PARAM_Kantar_on));
static_assert(range(8, PARAM_Kantar_on0, PARAM_Kantar_on1));
static_assert(range(9, PARAM_Relay_on, PARAM_Relay_on));
static_assert(range(10, PARAM_Input1_LiveWire_on, PARAM_Input1_LiveWire_on));
static_assert(range(11, PARAM_Input1_Livewire_Discovery_Enabled, PARAM_Input1_Ravenna_Discovery_Enabled));
static_assert(PARAM_Input1_Dante_Discovery_Enabled == PARAM_Input1_Livewire_Discovery_Enabled + 1);
static_assert(range(12, NOSAVEPARAM_PollStsFileEnabled, NOSAVEPARAM_PollStsFileEnabled));
static_assert(range(13, PARAM_PollStsTimeSchedule, PARAM_PollStsTimeSchedule));
