// REPlusLights — CLightSource hand-builder
//
// =============================================================================
//  Builds a 0x1C0-byte CLightSource from scratch, replicating exactly what
//  Legacy's CLightSource::Reset + SetCommon write.
// =============================================================================
//  WHY BY HAND. Enhanced inlined its constructor — its light producers call no
//  SetCommon at all, only two tiny helpers. There is no function to call there,
//  the same problem REPlus hit with the shape-test descriptor:
//
//      "Enhanced has it inlined into the only function that uses it, so there
//       is no call to reach. Every value below is what that ctor writes, read
//       off the Legacy copy."
//
//  So Legacy is the Rosetta stone: its Reset (RVA 0x21744) and SetCommon
//  (0x22F04) are real, non-inlined functions, and the struct is byte-identical
//  between builds (0x1C0, same offsets). Decompile there, replicate here, and
//  the result works on both.
//
//  Every constant below is either read out of those two functions or confirmed
//  against a real light captured in-game by the recon spike. Where the two
//  disagree, the comment says so.
//
//  POINT AND SPOT. The spot fields come from decompiling Legacy's
//  SetDirTangent (0x2379C) and SetSpotlight (0x25C54), then checking every
//  one against a real spot light captured in-game — the two agree exactly, so
//  the cone maths here is verified rather than inferred. Capsule shares the
//  spot path (it uses the same direction/tangent basis) but is otherwise
//  untested.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace lightbuild {

constexpr uint32_t kLightSize = 0x1C0;

enum LightType : int {
  kPoint = 1,
  kSpot = 2,
  kCapsule = 4,
  kDirectional = 8,
  // Not exhaustive: a type 16 exists in the wild, outside the four the
  // deferred renderer has techniques for.
};

// Flags dword at +0x64. Names from the game's own light-flag table; the bit
// numbers are (FLAG n - 1) since that table is 1-indexed.
//
// Cross-checked against the reverse engineering and they agree exactly:
//
//   - SetCommon's fixup keys on mask 0x2004018 = VEHICLE | FX | INT_AND_EXT |
//     CUTSCENE. So the fixup is interior/exterior classification: a light that
//     is none of those special kinds gets defaulted to EXTERIOR_ONLY, and one
//     that is has both interior/exterior bits cleared.
//   - RenderLight masks with 0xFFFFFE3F to strip bits 6/7/8 on its
//     not-eligible path — exactly the three CAST_*_SHADOWS flags.
//   - A real captured spot light read 0x040001E8 = VEHICLE |
//     TEXTURE_PROJECTION | CAST_SHADOWS | CAST_STATIC | CAST_DYNAMIC |
//     MOVING_LIGHT_SOURCE. A car headlight, and every bit of that is coherent.
enum LightFlags : uint32_t {
  kInteriorOnly = 1u << 0,
  kExteriorOnly = 1u << 1,
  kDisableInCutscene = 1u << 2,
  kVehicle = 1u << 3,
  // Also the blackout exemption. The per-light render (leg 0x3A7154, enh
  // 0x38F140) opens with a gate on the artificial-lights-off global - what a
  // trainer's blackout and SET_ARTIFICIAL_LIGHTS_STATE drive - and while it is
  // set a light renders only if it carries FX, or VEHICLE when the second
  // global is clear:
  //     if (blackout) { if ((flags & (vehicleAllowed ? 0x18 : 0x10)) == 0) return; }
  // So a scene light placed in a blacked-out city needs this bit, and that is
  // the entire fix - the engine is doing what it was told, not failing.
  kFx = 1u << 4,
  kTextureProjection = 1u << 5,
  kCastShadows = 1u << 6,
  kCastStaticShadows = 1u << 7,
  kCastDynamicShadows = 1u << 8,
  kCalcFromSunlight = 1u << 9,
  kEnableBuzzing = 1u << 10,
  kForceBuzzing = 1u << 11,
  kVolumeDrawing = 1u << 12,
  kNoSpecular = 1u << 13,
  kIntAndExt = 1u << 14,
  kOnlyCorona = 1u << 15,
  kDontRenderInReflection = 1u << 16,
  kOnlyRenderInReflection = 1u << 17,
  kUseCullPlane = 1u << 18,
  kUseVolOuterColour = 1u << 19,
  // Doubles the light's PRIORITY for a shadow slot, not just its resolution.
  //
  // Only eight lights in a scene get a shadow map. The manager scores every
  // candidate as `range / distance`, sorts, and takes the top eight - and
  // applies a x2.0 multiplier (leg DAT_7ff6808775dc) to anything carrying this
  // bit, AFTER the score is clamped. Measured 66 eligible lights in the tuners
  // car meet against 3 in an apartment, which is the whole reason a light that
  // shadows fine in one interior casts nothing in the other.
  //
  // So this and Range are the two levers on whether a light wins a slot. Do NOT
  // reach for raising the cap instead: there are exactly eight shadow records
  // allocated (leg 0x7ff6811e1520..0x7ff6811e1ea0, 8 x 0x130), so a bigger
  // limit writes past the end of them.
  kCastHigherResShadow = 1u << 20,
  kCastOnlyLowResShadows = 1u << 21,
  kAddToVipLodLights = 1u << 22,
  kDontLightAlpha = 1u << 23,
  kShadowsIfPossible = 1u << 24,
  // Also the interior-shadow switch. UNPROVEN, and opt-in for that reason:
  // it was added believing interiors broke shadows generally, and they do not -
  // a small interior shadows fine with this clear. The real cause of a light
  // not shadowing in a BIG interior is the eight-slot cap; see kCastHigherResShadow.
  // The mechanism below is real, it just was not the bug it was written for.
  //
  // The shadow manager reads the light's INTERIOR LOCATION from +0x94 and files
  // it into the shadow record, and that is what decides whether an interior's
  // rooms are drawn into the shadow map. We always write the invalid sentinel
  // there (0xFFFCFFFF - fwInteriorLocation::IsValid, leg 0x5469C4, rejects a
  // low half of -1), because we have no entity to inherit one from.
  //
  // The engine resolves it from the light's POSITION instead - but only for a
  // light carrying this bit:
  //     if (flags & CUTSCENE) {
  //         if (!IsValid(loc) && !(flags & EXTERIOR_ONLY)) loc = ResolveFrom(pos);
  //     }
  //     shadowRecord[+0x120] = loc;
  // which is exactly our case: a light placed by script that no interior owns.
  // Without it a light inside a building casts against the exterior world only,
  // so its shadows simply do not appear indoors.
  //
  // It also puts the light on the not-culled path in the per-light render
  // (`flags & 0x2000010`, shared with FX), which is in our favour.
  kCutscene = 1u << 25,
  kMovingLightSource = 1u << 26,
  kUseVehicleTwin = 1u << 27,
  kForceMediumLodLight = 1u << 28,
  kCoronaOnlyLodLight = 1u << 29,
};

// Time flags for +0x6C: bits 0..23 are the 24 hours the light is lit.
constexpr uint32_t kTimeAllHours = 0x00FFFFFFu;

// Base for the synthetic shadow-owner token written to +0x88. Windows never
// maps the low 64K, so the engine's own token (an entity pointer scaled by 16)
// is always >= 0x100000; staying well below that cannot collide with a game
// light. See the shadow identity block in BuildLight for why this is needed.
constexpr uint64_t kShadowOwnerBase = 0x1000ull;

struct LightParams {
  LightType type = kPoint;

  float pos[3] = {0, 0, 0};
  float colour[3] = {1, 1, 1}; // 0..1
  float intensity = 10.0f;
  float range = 40.0f;  // metres
  float falloff = 8.0f; // exponent; must not be 0
  uint32_t timeFlags = kTimeAllHours; // lit at every hour

  // ---- Spot only ----
  //
  // `dir` is normalized on write. `tangent` must be perpendicular to it; pass
  // all-zero and one is derived (see BuildLight). Angles in DEGREES: the game
  // clamps outer to [1, 89] and inner to [1, outer-1], and stores both the
  // degrees and their cosines.
  float dir[3] = {0, 0, -1};
  float tangent[3] = {0, 0, 0};
  float innerAngle = 10.0f;
  float outerAngle = 45.0f;

  // ---- Volumetric ----
  //
  // Only used when kVolumeDrawing is set. The engine's entity-light builder
  // (leg 0x4116D0) writes these three from the light's volume attributes on its
  // volumetric branch, and ZEROES the first two on the other one - see
  // BuildLight, which reproduces both sides. Defaults match Reset.
  float volIntensity = 1.0f;
  float volSize      = 1.0f;
  float volExponent  = 1.0f;

  // Flags, fed through SetCommon's fixup (see LightFlags above).
  //
  // Defaults to INT_AND_EXT deliberately. Passing 0 makes the fixup classify
  // the light as EXTERIOR_ONLY, which is what every earlier build of this
  // project did — and it means the light would not render indoors. Nothing
  // caught that, because every test so far happened outdoors.
  //
  // Add kCastShadows (| kCastStaticShadows | kCastDynamicShadows) for a
  // shadow-casting light.
  uint32_t flags = kIntAndExt;
};

// Kept so existing point-only call sites still compile.
using PointLightParams = LightParams;

namespace detail {

inline void W32(uint8_t* p, uint32_t off, uint32_t v) {
  std::memcpy(p + off, &v, sizeof(v));
}
inline void WF(uint8_t* p, uint32_t off, float v) {
  std::memcpy(p + off, &v, sizeof(v));
}
inline void W64(uint8_t* p, uint32_t off, uint64_t v) {
  std::memcpy(p + off, &v, sizeof(v));
}

inline float Clamp(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Normalize in place; returns false if the vector was degenerate.
inline bool Normalize(float v[3]) {
  const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (len2 <= 0.0f)
    return false;
  const float inv = 1.0f / std::sqrt(len2);
  v[0] *= inv;
  v[1] *= inv;
  v[2] *= inv;
  return true;
}

} // namespace detail

// `out` must be at least kLightSize bytes and 16-byte aligned.
//
// `shadowId` identifies this light to the shadow manager. It only has to be
// distinct per light and the SAME every frame for a given light - the caller's
// stable index into the light set is exactly right. See the shadow identity
// block below.
inline void BuildLight(uint8_t* out, const LightParams& p,
                       uint32_t shadowId = 0) {
  using detail::W32;
  using detail::W64;
  using detail::WF;

  // Reset starts from a zeroed object for our purposes: every field it does
  // not explicitly write, the game leaves at whatever the slot held. We cannot
  // rely on that, so zero first and write only what Reset actually sets.
  std::memset(out, 0, kLightSize);

  // ---- CLightSource::Reset (Legacy 0x21744) -----------------------------
  //
  // +0x2C and +0x3C are left at zero here. Ghidra could not resolve what Reset
  // writes to them, and a captured light showed they are the `w` lanes of the
  // direction (+0x20) and tangent (+0x30) vectors, carrying an integer id
  // rather than a number. Zero is what an unwritten field looks like and is
  // what a type-16 light had; a spot light had 0x1DA. For a point light,
  // neither vector is used, so zero is the honest value.
  WF(out, 0x28, 1.0f);
  WF(out, 0x34, 1.0f);
  WF(out, 0x40, 1.0f); // Reset copies two 8-byte constants here; both are
  WF(out, 0x44, 1.0f); // 1.0f pairs, confirmed against a captured light
  WF(out, 0x48, 1.0f);
  WF(out, 0x4C, 1.0f);
  W32(out, 0x50, 0xFFFFFFFFu);
  W32(out, 0x54, 0xFFFFFFFFu);
  W32(out, 0x58, 0xFFFFFFFFu);
  W32(out, 0x5C, 0xFFFFFFFFu);
  out[0x6F] = 0;
  W32(out, 0x70, 0xFFFFFFFFu);
  WF(out, 0x78, 1.0f);
  WF(out, 0x7C, 1.0f);
  WF(out, 0x80, 1.0f);
  W32(out, 0x90, 0xFFFFFFFFu);
  W32(out, 0x94, 0xFFFCFFFFu); // what the tiny helper at Enh 0x93F540 writes
  WF(out, 0x98, 1.0f);         // range; overwritten below
  WF(out, 0xD4, 0.01f);
  WF(out, 0xDC, 1.0f);
  WF(out, 0xE0, 1.0f);
  W32(out, 0x1B0, 0); // tail-array element count

  // ---- CLightSource::SetCommon (Legacy 0x22F04) -------------------------
  //
  // Position and colour are Vec4 — SetCommon copies FOUR dwords from each
  // (`param_1[3] = param_4[3]`), not three. The `w` lanes are left at zero:
  // captured lights show w varying (569.485 on one, 0.0 on another), and the
  // Legacy injection spike rendered correctly for 54k frames with
  // uninitialized stack in that lane, so it is not load-bearing for a point
  // light.
  WF(out, 0x00, p.pos[0]);
  WF(out, 0x04, p.pos[1]);
  WF(out, 0x08, p.pos[2]);
  WF(out, 0x10, p.colour[0]);
  WF(out, 0x14, p.colour[1]);
  WF(out, 0x18, p.colour[2]);

  W32(out, 0x60, static_cast<uint32_t>(p.type));

  // SetCommon's flags fixup, replicated exactly:
  //     if ((f & 0x2004018) == 0) f = (f & ~1) | 2;
  //     else                      f =  f & ~3;
  // Starting from 0, that yields 2 — which is what a captured light's low bits
  // showed.
  {
    // Run the fixup on the caller's bits, exactly as the game would: it keys
    // on whether any of 0x2004018 is set, so extraFlags genuinely changes the
    // outcome rather than just being OR'd on afterwards.
    uint32_t f = p.flags;
    if ((f & 0x2004018u) == 0)
      f = (f & ~1u) | 2u;
    else
      f = f & ~3u;
    W32(out, 0x64, f);
  }

  WF(out, 0x68, p.intensity);
  // Bit 29 is not a time flag - it is the fade solver's "use these values as
  // the fades" switch, and it is what keeps a scene light's shadows alive.
  //
  // The solver (leg 0x3BC2A8, first thing the per-light render calls) normally
  // turns the four bytes at +0xD0..+0xD3 into DISTANCES and compares them
  // against the light-to-camera distance. On the way it scales all four down by
  // a global multiplier whenever the light's interior location at +0x94 is
  // invalid - and ours always is, because no entity owns us. In a large
  // interior that scaled distance drops under the camera distance, the shadow
  // fade evaluates to 0, and the render answers with
  //     flags &= 0xFFFFFE3F
  // which erases CAST_SHADOWS, CAST_STATIC and CAST_DYNAMIC in place. Measured:
  // we hand it 0x020041C0 and the shadow candidate list holds 0x82004000. By
  // then the shadow manager cannot score the light at all, which is why no
  // Shadow Quality setting made any difference - the bits it acts on were gone.
  //
  // With this bit set the solver returns the raw table values instead and
  // returns early, before both the scaling and the distance test. Paired with
  // the 255s written below that is 1.0 on all four lanes: no distance fade on
  // the light, its shadow, its volume or its corona. For a light someone placed
  // by hand that is the wanted behaviour anyway.
  constexpr uint32_t kFadeOverride = 1u << 29;
  W32(out, 0x6C, (p.timeFlags & kTimeAllHours) | kFadeOverride);

  // SetCommon memsets +0x9C..+0xCF then writes the falloff default. Already
  // zero from the memset above; write the default so the intent is visible.
  WF(out, 0x9C, 8.0f);

  // ---- What the producers then override ---------------------------------
  WF(out, 0x98, p.range);
  WF(out, 0x9C, p.falloff > 0.0f ? p.falloff : 8.0f);

  // ---- CLightSource::SetDirTangent + SetSpotlight ------------------------
  //
  // Both decompiled from Legacy (0x2379C, 0x25C54) and then checked field for
  // field against a real spot light captured in-game: inner 1.0 deg, outer
  // 60.21 deg, range 135 gave cos(outer)=0.4968, sin(outer)*range=117.2,
  // cos(outer)*range=67.07 — matching the capture exactly on all six.
  //
  // MUST come after the range write above: +0xC0/+0xC4 are derived from it.
  if (p.type == kSpot || p.type == kCapsule) {
    float dir[3] = {p.dir[0], p.dir[1], p.dir[2]};
    if (!detail::Normalize(dir)) {
      dir[0] = 0.0f;
      dir[1] = 0.0f;
      dir[2] = -1.0f; // straight down, the least surprising default
    }

    // SetDirTangent normalizes both vectors but does NOT orthogonalise them —
    // the game always passes a genuine perpendicular. Derive one rather than
    // trusting a caller to, since a tangent parallel to dir would give a
    // degenerate cone basis.
    float tan[3] = {p.tangent[0], p.tangent[1], p.tangent[2]};
    if (!detail::Normalize(tan) ||
        std::fabs(tan[0] * dir[0] + tan[1] * dir[1] + tan[2] * dir[2]) >
            0.99f) {
      // Cross dir with whichever axis it is least aligned to.
      const float up[3] = {0.0f, 0.0f, 1.0f};
      const float alt[3] = {1.0f, 0.0f, 0.0f};
      const float* a = (std::fabs(dir[2]) > 0.99f) ? alt : up;
      tan[0] = dir[1] * a[2] - dir[2] * a[1];
      tan[1] = dir[2] * a[0] - dir[0] * a[2];
      tan[2] = dir[0] * a[1] - dir[1] * a[0];
      detail::Normalize(tan);
    }

    WF(out, 0x20, dir[0]);
    WF(out, 0x24, dir[1]);
    WF(out, 0x28, dir[2]);
    WF(out, 0x30, tan[0]);
    WF(out, 0x34, tan[1]);
    WF(out, 0x38, tan[2]);

    // Clamps are the game's own: outer to [1, 89], inner to [1, outer-1].
    // 89.0f is a literal read out of the Legacy binary, not a guess.
    const float outer = detail::Clamp(p.outerAngle, 1.0f, 89.0f);
    const float inner = detail::Clamp(p.innerAngle, 1.0f, outer - 1.0f);

    constexpr float kDeg2Rad = 0.017453292f; // the constant SetSpotlight uses
    const float cosInner = std::cos(inner * kDeg2Rad);
    const float cosOuter = std::cos(outer * kDeg2Rad);
    const float sinOuter = std::sqrt(1.0f - cosOuter * cosOuter);

    WF(out, 0xB0, cosInner);
    WF(out, 0xB4, cosOuter);
    WF(out, 0xB8, inner);
    WF(out, 0xBC, outer);
    WF(out, 0xC0, sinOuter * p.range); // cone radius at max distance
    WF(out, 0xC4, cosOuter * p.range); // cone depth
  }

  // ---- Fade distances (+0xD0..+0xD3) -------------------------------------
  //
  // Four bytes, and each one is a distance IN METRES. The fade solver (leg
  // 0x3BC2A8, called first thing by the per-light render) reads each as an
  // index into a 256-entry i/255 table and multiplies by 255.0, so the byte and
  // the metre are the same number. They gate, in order:
  //
  //     +0xD0  the light itself   - fade 0 and it is not rendered at all
  //     +0xD1  its SHADOW         - fade 0 and the render does
  //                                 `flags &= 0xFFFFFE3F`, wiping CAST_SHADOWS,
  //                                 CAST_STATIC and CAST_DYNAMIC in place
  //     +0xD2  volume             - fade 0 sets flags |= 0x2000
  //     +0xD3  corona             - fade 0 clears the blur bit at +0x6F
  //
  // We left all four at zero, which falls through to a short engine default. It
  // is generous enough that a light shadows fine in a small room and outdoors
  // up close, and short enough that in a large interior the camera is simply
  // beyond it - at which point the shadow flags are gone BEFORE the shadow
  // manager ever scores the light. That is why it read "present but not
  // eligible" there, and why no Shadow Quality setting made any difference:
  // the bits it works on had already been stripped.
  //
  // 255 m is the table's ceiling and means "do not fade" for a scene light.
  out[0xD0] = 0xFF;
  out[0xD1] = 0xFF;
  out[0xD2] = 0xFF;
  out[0xD3] = 0xFF;

  // ---- Volume (+0x78/+0x7C/+0x80) ----------------------------------------
  //
  // The entity-light builder branches on VOLUME_DRAWING and writes these three
  // from the light's attributes, or zeroes the first two and leaves the third
  // at 1.0 when the flag is clear. Reset alone leaves all three at 1.0, so a
  // non-volumetric light built here did not match one the game built - harmless
  // while the renderer gates on the flag, but there is no reason to differ.
  if (p.flags & kVolumeDrawing) {
    WF(out, 0x78, p.volIntensity);
    WF(out, 0x7C, p.volSize);
    WF(out, 0x80, p.volExponent);
  } else {
    WF(out, 0x78, 0.0f);
    WF(out, 0x7C, 0.0f);
    WF(out, 0x80, 1.0f);
  }

  // ---- Shadow identity (+0x88) -------------------------------------------
  //
  // Setting the CAST_*_SHADOWS flags is NOT enough on its own, and this is the
  // reason. The per-frame flip that feeds the shadow manager - Legacy
  // FUN_7ff67f42bf4c, Enhanced FUN_7ff7a45730a0, the same code on both builds -
  // walks every light that rendered last frame and copies it into the shadow
  // candidate list only `if (light[+0x88] != 0)`. A light with the flags set
  // but no owner is dropped before the shadow manager ever looks at it, with no
  // other symptom at all: it still lights the scene, it just never casts. Every
  // downstream check (type, flags, distance) is fine; it is simply not there.
  //
  // The engine's own value is `entityPtr * 0x10 + lightIndex` (Legacy
  // 0x3B9B5C). A pointer scaled by 16 cannot be an address, and every use found
  // only ever compares or stores it: the manager keys a light to its shadow
  // slot with it (0x450B18) and copies it into the slot record at +0x110
  // (0x42FB4C). So it is an opaque identity token, and a synthetic one works.
  //
  // It must be stable across frames. If it changes, the manager cannot match
  // the light to the slot it had last frame and re-allocates one every frame.
  //
  // Gated on the flag the same way the engine gates it, so a light that is not
  // meant to cast does not take up room in the candidate list.
  if (p.flags & kCastShadows)
    W64(out, 0x88, kShadowOwnerBase + shadowId);

  // Every Enhanced producer sets this bit immediately before appending
  // (`OR byte ptr [light+0x67], 0x80`). +0x67 is the top byte of the flags
  // dword at +0x64, so this turns 0x00000002 into 0x80000002 — exactly the
  // value observed on a real captured light.
  out[0x67] |= 0x80;
}

// Point-only convenience, kept so existing call sites still compile.
inline void BuildPointLight(uint8_t* out, const LightParams& p,
                            uint32_t shadowId = 0) {
  LightParams q = p;
  q.type = kPoint;
  BuildLight(out, q, shadowId);
}

} // namespace lightbuild
