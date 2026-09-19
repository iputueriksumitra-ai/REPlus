// This file is part of RockstarEditorPlus.
// WOW TEAM SCENE extension: Cloud Hat override for Rockstar Editor playback.
// SPDX-License-Identifier: GPL-3.0-only

#include "main.h"
#include "replay/cloudhat.h"

#include <cstdint>
#include <cstring>

namespace cloudhat
{
    namespace
    {
        using NativeInitFn   = void (*)(std::uint64_t);
        using NativePush64Fn = void (*)(std::uint64_t);
        using NativeCallFn   = std::uint64_t* (*)();

        NativeInitFn   s_nativeInit   = nullptr;
        NativePush64Fn s_nativePush64 = nullptr;
        NativeCallFn   s_nativeCall   = nullptr;
        bool s_bound = false;

        // Stable native hashes used by FiveM/ScriptHookV.
        constexpr std::uint64_t H_LOAD_CLOUD_HAT       = 0xFC4842A34657BFCBull;
        constexpr std::uint64_t H_UNLOAD_ALL_CLOUD_HATS= 0x957E790EA1727B64ull;
        constexpr std::uint64_t H_SET_CLOUDS_ALPHA     = 0xF36199225D6D8C86ull;
        constexpr std::uint64_t H_GET_CLOUDS_ALPHA     = 0x20AC25E781AE4A84ull;

        const char* const kCloudHatNames[] = {
            "altostratus",
            "Cirrus",
            "cirrocumulus",
            "Clear 01",
            "Cloudy 01",
            "Contrails",
            "Horizon",
            "horizonband1",
            "horizonband2",
            "horizonband3",
            "horsey",
            "Nimbus",
            "Puffs",
            "RAIN",
            "Snowy 01",
            "Stormy 01",
            "stratoscumulus",
            "Stripey",
            "shower",
            "Wispy",
        };

        bool s_wasEdit = false;
        bool s_appliedOverride = false;
        int  s_appliedType = -1;
        float s_appliedAlpha = -1.0f;
        float s_alphaBeforeOverride = 1.0f;
        bool s_haveSavedAlpha = false;

        std::uint64_t packFloat(float v)
        {
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
            std::memcpy(&bits, &v, sizeof(bits));
            return (std::uint64_t)bits;
        }

        void begin(std::uint64_t hash)
        {
            s_nativeInit(hash);
        }

        void push(std::uint64_t value)
        {
            s_nativePush64(value);
        }

        std::uint64_t* call()
        {
            return s_nativeCall();
        }

        void invoke0(std::uint64_t hash)
        {
            begin(hash);
            call();
        }

        void invokeStringFloat(std::uint64_t hash, const char* text, float value)
        {
            begin(hash);
            push((std::uint64_t)(std::uintptr_t)text);
            push(packFloat(value));
            call();
        }

        void invokeFloat(std::uint64_t hash, float value)
        {
            begin(hash);
            push(packFloat(value));
            call();
        }

        float invokeFloatResult(std::uint64_t hash, float fallback)
        {
            begin(hash);
            std::uint64_t* result = call();
            if (!result) return fallback;
            const std::uint32_t bits = (std::uint32_t)(*result & 0xFFFFFFFFu);
            float v = fallback;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }

        FARPROC findExport(HMODULE shv, const char* plain, const char* decorated)
        {
            if (!shv) return nullptr;
            if (FARPROC p = GetProcAddress(shv, plain)) return p;
            return GetProcAddress(shv, decorated);
        }

        void clearAppliedState(bool restoreAlpha)
        {
            if (!s_bound) return;

            // Only undo something this extension actually applied. On an
            // untouched clip "As Recorded" must remain a true no-op.
            if (s_appliedOverride)
            {
                invoke0(H_UNLOAD_ALL_CLOUD_HATS);
                if (restoreAlpha && s_haveSavedAlpha)
                    invokeFloat(H_SET_CLOUDS_ALPHA, s_alphaBeforeOverride);
            }

            s_appliedOverride = false;
            s_appliedType = -1;
            s_appliedAlpha = -1.0f;
            s_haveSavedAlpha = false;
        }
    }

    bool bindScriptHook(HMODULE shv)
    {
        if (s_bound) return true;
        if (!shv) return false;

        s_nativeInit = reinterpret_cast<NativeInitFn>(findExport(
            shv, "nativeInit", "?nativeInit@@YAX_K@Z"));
        s_nativePush64 = reinterpret_cast<NativePush64Fn>(findExport(
            shv, "nativePush64", "?nativePush64@@YAX_K@Z"));
        s_nativeCall = reinterpret_cast<NativeCallFn>(findExport(
            shv, "nativeCall", "?nativeCall@@YAPEA_KXZ"));

        s_bound = s_nativeInit && s_nativePush64 && s_nativeCall;
        if (s_bound)
            logger::write("info", "cloudhat: ScriptHookV native bridge ready");
        else
            logger::write("info", "cloudhat: ScriptHookV native exports unavailable - Cloud Hat page disabled");
        return s_bound;
    }

    bool ready()
    {
        return s_bound;
    }

    int typeCount()
    {
        return (int)(sizeof(kCloudHatNames) / sizeof(kCloudHatNames[0]));
    }

    const char* typeName(int index)
    {
        if (index < 0 || index >= typeCount()) return "?";
        return kCloudHatNames[index];
    }

    void tick()
    {
        if (!s_bound) return;

        const bool edit = game::isEditModeActive();
        if (!edit)
        {
            if (s_wasEdit)
                clearAppliedState(true);
            s_wasEdit = false;
            return;
        }

        const Config& c = Config::get();
        const bool want = c.overrideCloudHat;
        int type = c.cloudHatType;
        if (type < 0) type = 0;
        if (type >= typeCount()) type = typeCount() - 1;
        float alpha = c.cloudHatOpacity;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        // First editor frame or transition from As Recorded to an override.
        if (want && (!s_wasEdit || !s_appliedOverride))
        {
            s_alphaBeforeOverride = invokeFloatResult(H_GET_CLOUDS_ALPHA, 1.0f);
            s_haveSavedAlpha = true;
        }

        if (!want)
        {
            if (s_appliedOverride)
                clearAppliedState(true);
            s_wasEdit = true;
            return;
        }

        // IMPORTANT: LOAD_CLOUD_HAT is NOT a heartbeat. Repeated cloud-state
        // writes can disturb natural cloud animation in graphics mods. Apply
        // only on editor entry / enable / actual Cloud Hat selection change.
        if (!s_appliedOverride || s_appliedType != type)
        {
            invokeStringFloat(H_LOAD_CLOUD_HAT, typeName(type), 0.0f);
            s_appliedType = type;
            s_appliedOverride = true;
            logger::write("info", "cloudhat: loaded '%s' for Rockstar Editor playback", typeName(type));
        }

        if (s_appliedAlpha < 0.0f || fabsf(s_appliedAlpha - alpha) > 0.0001f)
        {
            invokeFloat(H_SET_CLOUDS_ALPHA, alpha);
            s_appliedAlpha = alpha;
        }

        s_wasEdit = true;
    }
}
