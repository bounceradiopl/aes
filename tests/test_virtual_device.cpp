// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/virtual_device.hpp"

using bam::classify_virtual_device;

TEST_CASE("VB-Audio cables are paired by their adapter name") {
  auto in = classify_virtual_device("CABLE Input (VB-Audio Virtual Cable)");
  auto out = classify_virtual_device("CABLE Output (VB-Audio Virtual Cable)");
  REQUIRE(in);
  REQUIRE(out);
  CHECK(in->family == "VB-CABLE");
  CHECK(in->cable == "CABLE");
  CHECK_FALSE(in->pair_key.empty());
  CHECK(in->pair_key == out->pair_key);

  auto a_in = classify_virtual_device("CABLE-A Input (VB-Audio Cable A)");
  auto a_out = classify_virtual_device("CABLE-A Output (VB-Audio Cable A)");
  auto b_in = classify_virtual_device("CABLE-B Input (VB-Audio Cable B)");
  REQUIRE(a_in);
  REQUIRE(a_out);
  REQUIRE(b_in);
  CHECK(a_in->cable == "CABLE-A");
  CHECK(b_in->cable == "CABLE-B");
  CHECK(a_in->pair_key == a_out->pair_key);
  CHECK(a_in->pair_key != b_in->pair_key);
  CHECK(a_in->pair_key != in->pair_key);

  auto hifi = classify_virtual_device("Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)");
  REQUIRE(hifi);
  CHECK(hifi->cable == "Hi-Fi Cable");
  CHECK_FALSE(hifi->pair_key.empty());
}

TEST_CASE("a renamed VB-CABLE endpoint is still recognised") {
  // Windows lets the user rename the part before the brackets; the adapter name stays.
  auto v = classify_virtual_device("mAirList PGM (VB-Audio Virtual Cable)");
  REQUIRE(v);
  CHECK(v->cable == "CABLE");
  CHECK(v->pair_key == classify_virtual_device("CABLE Output (VB-Audio Virtual Cable)")->pair_key);
  CHECK(classify_virtual_device("cable input (vb-audio virtual cable)"));  // case does not matter
}

TEST_CASE("Virtual Audio Cable lines pair by line name") {
  // VAC names both ends of a line alike: "Line 1 (Virtual Audio Cable)"
  auto l1 = classify_virtual_device("Line 1 (Virtual Audio Cable)");
  auto l2 = classify_virtual_device("Line 2 (Virtual Audio Cable)");
  REQUIRE(l1);
  REQUIRE(l2);
  CHECK(l1->family == "Virtual Audio Cable");
  CHECK(l1->cable == "Line 1");
  CHECK_FALSE(l1->pair_key.empty());
  CHECK(l1->pair_key != l2->pair_key);
}

TEST_CASE("other virtual devices are tagged but not paired") {
  struct Case {
    const char* name;
    const char* family;
  };
  for (const Case& c : {Case{"Voicemeeter Input (VB-Audio Voicemeeter VAIO)", "Voicemeeter"},
                        Case{"Voicemeeter Out B1 (VB-Audio Voicemeeter VAIO)", "Voicemeeter"},
                        Case{"Speakers (Virtual Audio Driver)", "Virtual Audio Driver"},
                        Case{"Microphone (Virtual Mic Driver)", "Virtual Audio Driver"},
                        Case{"Speakers (Steam Streaming Speakers)", "Steam Streaming"},
                        Case{"Microphone (Steam Streaming Microphone)", "Steam Streaming"},
                        Case{"Speakers (Scream (WDM))", "Scream"}}) {
    auto v = classify_virtual_device(c.name);
    REQUIRE_MESSAGE(v, c.name);
    CHECK_MESSAGE(v->family == c.family, c.name);
    CHECK_MESSAGE(v->cable.empty(), c.name);
    CHECK_MESSAGE(v->pair_key.empty(), c.name);
  }
}

TEST_CASE("hardware devices are not virtual") {
  for (const char* name : {"Głośniki (Realtek(R) Audio)", "Mikrofon (USB Audio CODEC)", "Speakers (RME Fireface UCX II)",
                           "Cable Modem Speaker", "Virtual Speakers", ""}) {
    CHECK_FALSE_MESSAGE(classify_virtual_device(name), name);
  }
}
