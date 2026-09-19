// Rockstar Editor+ — scene light model
//
// Originally developed standalone (REPlusLights); folded into REPlus so the
// lights live alongside the camera and render work rather than in a second ASI
// that would have to coexist with this one.
//
// =============================================================================
//  A set of scene lights, loaded from INI, published to the render hook.
// =============================================================================
//  Replaces the single hardcoded light the spikes carried while the engine work
//  was being proved out. Everything here is game-agnostic: it produces
//  lightbuild::LightParams, and the per-build injection code decides how to get
//  those into the engine.
//
//  THREADING. The hook reads this from the game's render path while the config
//  thread reloads the INI. A std::vector would be a use-after-free waiting to
//  happen, so the set is published as a fixed-size POD snapshot behind an
//  atomic index: the writer fills the inactive buffer and flips, the reader
//  takes the active one. No locks on the hot path, and the reader can never see
//  a half-written set — at worst it uses the previous one for another frame.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "utils/log.h"

#include "lightbuild.h"

namespace lightmodel {

// Generous: the Legacy list grows on demand and Enhanced's own per-frame list
// does too, so this bounds our INI, not the engine.
constexpr int kMaxLights = 32;

struct Entry {
  bool enabled = true;
  lightbuild::LightParams p;
};

struct Snapshot {
  int count = 0;
  Entry lights[kMaxLights];
};

namespace detail {

inline Snapshot g_buffers[2];
inline std::atomic<int> g_active{0};

inline float IniFloat(const char* ini, const char* section, const char* key,
                      float fallback) {
  char buf[64] = {0};
  char def[64];
  _snprintf_s(def, sizeof(def), _TRUNCATE, "%g", fallback);
  GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), ini);

  // strtod rather than atof: atof turns a typo into 0.0, which for a coordinate
  // means the light silently moves hundreds of metres and reads as a
  // malfunction rather than a typo. That cost an hour once already.
  char* end = nullptr;
  const double v = strtod(buf, &end);
  if (end == buf) {
    logger::write("info", "lights: [%s] %s=\"%s\" is not a number - using %g",
                  section, key, buf, fallback);
    return fallback;
  }

  // Trailing junk is accepted by strtod but almost never intended: "-2117.56.0"
  // quietly becomes -2117.56, and a value that is nearly right is harder to
  // spot than one that is obviously wrong. Use what parsed, but say so.
  while (*end == ' ' || *end == '\t')
    ++end;
  if (*end != '\0') {
    logger::write("info",
                  "lights: [%s] %s=\"%s\" has trailing characters - read as %g",
                  section, key, buf, v);
  }
  return static_cast<float>(v);
}

// Accepts decimal or 0x-prefixed hex, so flags can be written either way.
inline uint32_t IniFlags(const char* ini, const char* section, const char* key,
                         uint32_t fallback) {
  char buf[64] = {0};
  GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ini);
  if (!buf[0])
    return fallback;
  char* end = nullptr;
  const unsigned long v = strtoul(buf, &end, 0);
  return (end == buf) ? fallback : static_cast<uint32_t>(v);
}

} // namespace detail

// Read the whole set from `iniPath` and publish it.
//
// Layout:
//   [Lights]  Count=N
//   [Light0]  Enabled, Type, X/Y/Z, R/G/B, Intensity, Range, Falloff,
//             Pitch/Yaw, ConeInner/ConeOuter, Flags, CastShadows, ...
inline void Load(const char* iniPath) {
  const int inactive = 1 - detail::g_active.load(std::memory_order_relaxed);
  Snapshot& s = detail::g_buffers[inactive];

  int count = GetPrivateProfileIntA("Lights", "Count", 0, iniPath);
  if (count < 0)
    count = 0;
  if (count > kMaxLights)
    count = kMaxLights;
  s.count = count;

  for (int i = 0; i < count; ++i) {
    char sec[32];
    _snprintf_s(sec, sizeof(sec), _TRUNCATE, "Light%d", i);

    Entry& e = s.lights[i];
    e = Entry{}; // reset to defaults so a partial section is still coherent
    lightbuild::LightParams& p = e.p;

    e.enabled = GetPrivateProfileIntA(sec, "Enabled", 1, iniPath) != 0;
    p.type = GetPrivateProfileIntA(sec, "Type", 0, iniPath) == 1
                 ? lightbuild::kSpot
                 : lightbuild::kPoint;

    p.pos[0] = detail::IniFloat(iniPath, sec, "X", 0.0f);
    p.pos[1] = detail::IniFloat(iniPath, sec, "Y", 0.0f);
    p.pos[2] = detail::IniFloat(iniPath, sec, "Z", 0.0f);

    p.colour[0] = detail::IniFloat(iniPath, sec, "R", 1.0f);
    p.colour[1] = detail::IniFloat(iniPath, sec, "G", 1.0f);
    p.colour[2] = detail::IniFloat(iniPath, sec, "B", 1.0f);

    p.intensity = detail::IniFloat(iniPath, sec, "Intensity", 10.0f);
    p.range = detail::IniFloat(iniPath, sec, "Range", 40.0f);
    p.falloff = detail::IniFloat(iniPath, sec, "Falloff", 8.0f);

    // Volumetric shape. Only used when the Volumetric flag is set, but
    // read unconditionally so toggling the flag does not lose them.
    p.volIntensity = detail::IniFloat(iniPath, sec, "VolumeIntensity", 1.0f);
    p.volSize      = detail::IniFloat(iniPath, sec, "VolumeSize", 1.0f);
    p.volExponent  = detail::IniFloat(iniPath, sec, "VolumeFalloff", 1.0f);

    // Aim as pitch/yaw, same convention as the free camera: yaw 0 faces +Y,
    // positive pitch tilts up. Converted to the direction vector the engine
    // wants; the tangent is left zero so BuildLight derives a perpendicular.
    if (p.type == lightbuild::kSpot) {
      const float pitch = detail::IniFloat(iniPath, sec, "Pitch", -90.0f);
      const float yaw = detail::IniFloat(iniPath, sec, "Yaw", 0.0f);
      const float DEG = 3.14159265f / 180.0f;
      const float yr = yaw * DEG, pr = pitch * DEG;
      p.dir[0] = -std::sin(yr) * std::cos(pr);
      p.dir[1] = std::cos(yr) * std::cos(pr);
      p.dir[2] = std::sin(pr);
      p.innerAngle = detail::IniFloat(iniPath, sec, "ConeInner", 10.0f);
      p.outerAngle = detail::IniFloat(iniPath, sec, "ConeOuter", 45.0f);
    }

    // Raw flags first, then the named conveniences OR on top — so `Flags` can
    // reach any bit while the common ones stay readable.
    p.flags = detail::IniFlags(iniPath, sec, "Flags", lightbuild::kIntAndExt);

    if (GetPrivateProfileIntA(sec, "CastShadows", 0, iniPath) != 0) {
      // All three together is what a real shadow-casting light carries — a
      // captured vehicle headlight had exactly this trio set.
      p.flags |= lightbuild::kCastShadows | lightbuild::kCastStaticShadows |
                 lightbuild::kCastDynamicShadows;
    }
    if (GetPrivateProfileIntA(sec, "Volumetric", 0, iniPath) != 0)
      p.flags |= lightbuild::kVolumeDrawing;
    if (GetPrivateProfileIntA(sec, "NoSpecular", 0, iniPath) != 0)
      p.flags |= lightbuild::kNoSpecular;
    if (GetPrivateProfileIntA(sec, "OnlyCorona", 0, iniPath) != 0)
      p.flags |= lightbuild::kOnlyCorona;

    p.timeFlags = detail::IniFlags(iniPath, sec, "TimeFlags",
                                   lightbuild::kTimeAllHours);
  }

  // Publish. Release so everything written above is visible to a reader that
  // acquires this index.
  detail::g_active.store(inactive, std::memory_order_release);
}

// Render-thread side. Returns the active snapshot; never null, empty until the
// first Load.
inline const Snapshot& Current() {
  return detail::g_buffers[detail::g_active.load(std::memory_order_acquire)];
}

// ---------------------------------------------------------------------------
//  Editing — MAIN THREAD ONLY (menu input runs there)
// ---------------------------------------------------------------------------
//  The published snapshot is immutable by design, so edits go to an
//  authoritative copy and are pushed across with Publish(). That keeps the
//  render side reading a set that is always internally consistent, rather than
//  one being mutated underneath it a field at a time.

namespace detail {
inline Snapshot g_edit;
inline bool g_editInit = false;

// Seed the editing copy from whatever was last loaded, once.
inline void ensureEdit() {
  if (g_editInit)
    return;
  g_edit = g_buffers[g_active.load(std::memory_order_acquire)];
  g_editInit = true;
}
} // namespace detail

inline int Count() {
  detail::ensureEdit();
  return detail::g_edit.count;
}

inline Entry& Mutable(int i) {
  detail::ensureEdit();
  if (i < 0) i = 0;
  if (i >= kMaxLights) i = kMaxLights - 1;
  return detail::g_edit.lights[i];
}

// Append a light and return its index, or -1 if the set is full.
inline int Add(const Entry& e) {
  detail::ensureEdit();
  if (detail::g_edit.count >= kMaxLights)
    return -1;
  detail::g_edit.lights[detail::g_edit.count] = e;
  return detail::g_edit.count++;
}

inline void Remove(int i) {
  detail::ensureEdit();
  if (i < 0 || i >= detail::g_edit.count)
    return;
  for (int j = i; j + 1 < detail::g_edit.count; ++j)
    detail::g_edit.lights[j] = detail::g_edit.lights[j + 1];
  --detail::g_edit.count;
}

// Replace the whole editing set.
//
// For the per-clip store, which exchanges the set wholesale rather than editing
// it a light at a time. Clamped rather than trusted: the set can come off disk.
inline void SetAll(const Snapshot& s) {
  detail::ensureEdit();
  detail::g_edit = s;
  if (detail::g_edit.count < 0)
    detail::g_edit.count = 0;
  if (detail::g_edit.count > kMaxLights)
    detail::g_edit.count = kMaxLights;
}

// The editing copy, for a caller that needs the whole set rather than one
// light. Read-only on purpose: mutate through Mutable/Add/Remove so Publish
// stays the single point at which the render side sees a change.
inline const Snapshot& Editing() {
  detail::ensureEdit();
  return detail::g_edit;
}

// Push a set straight to the render side, leaving the editing copy alone.
//
// For the animated path: the value on screen mid-blend belongs to no keyframe,
// so routing it through g_edit would let a scrub overwrite the keyframe the
// menu is editing with an interpolated one.
inline void PublishSet(const Snapshot& s) {
  const int inactive = 1 - detail::g_active.load(std::memory_order_relaxed);
  detail::g_buffers[inactive] = s;
  detail::g_active.store(inactive, std::memory_order_release);
}

// Push the editing copy to the render side.
inline void Publish() {
  detail::ensureEdit();
  const int inactive = 1 - detail::g_active.load(std::memory_order_relaxed);
  detail::g_buffers[inactive] = detail::g_edit;
  detail::g_active.store(inactive, std::memory_order_release);
}

// ---------------------------------------------------------------------------
//  Saving
// ---------------------------------------------------------------------------

namespace detail {
inline void WriteF(const char* ini, const char* sec, const char* key, float v) {
  char buf[64];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%g", v);
  WritePrivateProfileStringA(sec, key, buf, ini);
}
inline void WriteI(const char* ini, const char* sec, const char* key, int v) {
  char buf[32];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", v);
  WritePrivateProfileStringA(sec, key, buf, ini);
}
} // namespace detail

// Write one light's section. Per-light rather than the whole file because this
// runs on every arrow press — rewriting 32 sections to change one number would
// be a lot of disk for a value the user is still scrubbing through.
inline void SaveLight(const char* ini, int i) {
  detail::ensureEdit();
  if (i < 0 || i >= detail::g_edit.count)
    return;
  const Entry& e = detail::g_edit.lights[i];
  const lightbuild::LightParams& p = e.p;

  char sec[32];
  _snprintf_s(sec, sizeof(sec), _TRUNCATE, "Light%d", i);

  detail::WriteI(ini, sec, "Enabled", e.enabled ? 1 : 0);
  detail::WriteI(ini, sec, "Type", p.type == lightbuild::kSpot ? 1 : 0);
  detail::WriteF(ini, sec, "X", p.pos[0]);
  detail::WriteF(ini, sec, "Y", p.pos[1]);
  detail::WriteF(ini, sec, "Z", p.pos[2]);
  detail::WriteF(ini, sec, "R", p.colour[0]);
  detail::WriteF(ini, sec, "G", p.colour[1]);
  detail::WriteF(ini, sec, "B", p.colour[2]);
  detail::WriteF(ini, sec, "Intensity", p.intensity);
  detail::WriteF(ini, sec, "Range", p.range);
  detail::WriteF(ini, sec, "Falloff", p.falloff);
  detail::WriteF(ini, sec, "VolumeIntensity", p.volIntensity);
  detail::WriteF(ini, sec, "VolumeSize", p.volSize);
  detail::WriteF(ini, sec, "VolumeFalloff", p.volExponent);

  if (p.type == lightbuild::kSpot) {
    // Stored as pitch/yaw, which is what the INI and the menu speak; the
    // direction vector is derived from them on load.
    const float DEG = 180.0f / 3.14159265f;
    const float pitch = std::asin(p.dir[2] < -1.0f ? -1.0f
                                  : p.dir[2] > 1.0f ? 1.0f : p.dir[2]) * DEG;
    const float yaw = std::atan2(-p.dir[0], p.dir[1]) * DEG;
    detail::WriteF(ini, sec, "Pitch", pitch);
    detail::WriteF(ini, sec, "Yaw", yaw);
    detail::WriteF(ini, sec, "ConeInner", p.innerAngle);
    detail::WriteF(ini, sec, "ConeOuter", p.outerAngle);
  }

  // The raw value is the authoritative one: it round-trips every bit,
  // including the ones with no named key. The named keys below are written
  // alongside it purely so the file stays readable and hand-editable — Load
  // takes Flags as the base and lets the named keys OR on top, so the two can
  // never disagree in a way that loses a setting.
  {
    char hex[16];
    _snprintf_s(hex, sizeof(hex), _TRUNCATE, "0x%08X", p.flags);
    WritePrivateProfileStringA(sec, "Flags", hex, ini);
  }
  detail::WriteI(ini, sec, "CastShadows",
                 (p.flags & lightbuild::kCastShadows) ? 1 : 0);
  detail::WriteI(ini, sec, "Volumetric",
                 (p.flags & lightbuild::kVolumeDrawing) ? 1 : 0);
  detail::WriteI(ini, sec, "NoSpecular",
                 (p.flags & lightbuild::kNoSpecular) ? 1 : 0);
  detail::WriteI(ini, sec, "OnlyCorona",
                 (p.flags & lightbuild::kOnlyCorona) ? 1 : 0);
}

inline void SaveCount(const char* ini) {
  detail::ensureEdit();
  detail::WriteI(ini, "Lights", "Count", detail::g_edit.count);
}

inline void SaveAll(const char* ini) {
  detail::ensureEdit();
  SaveCount(ini);
  for (int i = 0; i < detail::g_edit.count; ++i)
    SaveLight(ini, i);
}

} // namespace lightmodel
