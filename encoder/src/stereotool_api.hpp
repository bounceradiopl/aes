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
// *value stays valid only until the next call into the instance: copied at once
using GetStsValueFn = bool (*)(gStereoTool*, int index, int subindex, const char** value);
using CheckLicenseValidFn = bool (*)(gStereoTool*);
using GetUnlicensedUsedFeaturesFn = bool (*)(gStereoTool*, char* text, int text_maxlen);

// enum ID of ParameterEnum.h 11.07 (positions; the enum grows between releases, so a new
// library version means checking these again: the SDK check does it at build time)
inline constexpr int kParamWebserverEnable = 524;  // PARAM_WebserverEnable, "Enable web interface" (0/1)
inline constexpr int kParamWebserverPort = 525;    // PARAM_WebserverPort, "Port"
inline constexpr int kSaveAllSettings = 20386;     // ID_SAVE_ALLSETTINGS: all but the configuration

// The web interface's own password protection (account 0, "Admin"). ID_SAVE_ALLSETTINGS neither
// saves nor loads these (the "[User rights]" section of a full .ini), so a preset never carries
// them; the binding sets them again after every load anyway.
inline constexpr int kParamPasswordProtectionAny = 7854;  // PARAM_PASSWORD_PROTECTIONON_ANY, "Password protection on" (0/1)
inline constexpr int kParamPasswordProtection0 = 7855;    // PARAM_PASSWORD_PROTECTIONON_0 (account 0)
inline constexpr int kParamPassword0 = 7865;              // PARAM_PASSWORD_STRING_0, "Password 0" (account 0's password, plain text)
inline constexpr int kParamJsonToken = 9134;              // PARAM_PASSWORD_JSON_TOKEN, "JSON token" (the JSON API's token)

// Stereo Tool's own network features besides the web interface: each switch (0/1) of a feature
// that sends to or listens on the network, or that loads settings by itself (behind the binding's
// back: a scheduled or polled .sts could switch the others on again). A .sts made with Stereo
// Tool elsewhere may switch any of them on (they are part of ID_SAVE_ALLSETTINGS: a stream to a
// production Icecast, a UECP listener ...), so the binding switches every one off after the
// instance is created and after every preset load. A switch the library does not have (the plugin
// of a platform without that feature: GetStsValue refuses it) cannot be on.
struct NetworkSwitches {
  int first;          // enum ID of the first switch
  int count;          // consecutive switches from `first` (an array of the feature in ParameterEnum.h)
  const char* what;   // Polish, for the failure text
};
inline constexpr NetworkSwitches kNetworkSwitches[] = {
    {5066, 1, "wyjście strumieniowe (Icecast/Shoutcast)"},   // PARAM_STREAMOUTPUT_Enabled
    {9507, 7, "wyjście strumieniowe (Icecast/Shoutcast)"},   // PARAM_STREAMOUTPUT_Enabled_0 .. _6
    {3838, 1, "serwer UECP (RDS)"},                          // PARAM_UECP_Enable
    {6344, 9, "serwer UECP (RDS)"},                          // PARAM_UECP_Enable_1 .. _9
    {4008, 1, "nadawanie MicroMPX"},                         // PARAM_UMPX_Encoder_Enabled
    {4217, 100, "nadawanie MicroMPX"},                       // PARAM_UMPX_Enabled_0 .. _99 (its outputs)
    {4646, 1, "odbiór MicroMPX"},                            // PARAM_UMPX_Decoder_Enabled
    {5894, 1, "Kantar"},                                     // PARAM_Kantar_on
    {8541, 2, "Kantar"},                                     // PARAM_Kantar_on0, PARAM_Kantar_on1
    {8090, 1, "przekaźnik RDS (relay)"},                     // PARAM_Relay_on
    {4011, 1, "wejście Livewire"},                           // PARAM_Input1_LiveWire_on
    {8297, 3, "wykrywanie Livewire/Dante/Ravenna (AES67)"},  // PARAM_Input1_{Livewire,Dante,Ravenna}_Discovery_Enabled
    {9989, 1, "odczyt ustawień z pliku (Poll STS)"},         // NOSAVEPARAM_PollStsFileEnabled ("Poll STS Enabled")
    {6681, 1, "harmonogram presetów"},                       // PARAM_PollStsTimeSchedule ("Scheduler Enabled")
};

}  // namespace bam::encoder::detail::st
