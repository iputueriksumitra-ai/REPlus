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
        // -----------------------------------------------------------------
        // Native backends
        // -----------------------------------------------------------------
        // V2 prefers FiveM's own scripting-gta bridge. ScriptHookV's nativeCall
        // intentionally rejects calls when networkInited is absent; Rockstar
        // Editor can live outside that state even though RE+ itself is active.
        // scripting-gta!ScriptEngine::CallNativeHandler has no such session gate
        // and ultimately calls rage::scrEngine::GetNativeHandler directly.

        struct CfxScriptContext
        {
            void* argumentBuffer;
            void* resultBuffer;
            int   numArguments;
            int   numResults;
        };
        static_assert(sizeof(CfxScriptContext) == 24, "FiveM ScriptContext layout changed");

        using CfxCallNativeFn = bool (*)(std::uint64_t, CfxScriptContext&);
        CfxCallNativeFn s_cfxCallNative = nullptr;
        bool s_cfxBindLogged = false;

        using NativeInitFn   = void (*)(std::uint64_t);
        using NativePush64Fn = void (*)(std::uint64_t);
        using NativeCallFn   = std::uint64_t* (*)();

        NativeInitFn   s_nativeInit   = nullptr;
        NativePush64Fn s_nativePush64 = nullptr;
        NativeCallFn   s_nativeCall   = nullptr;
        bool s_shvBound = false;

        // Stable original native hashes.
        constexpr std::uint64_t H_PRELOAD_CLOUD_HAT    = 0x11B56FBBF7224868ull;
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

        // -----------------------------------------------------------------
        // Cloud override state
        // -----------------------------------------------------------------
        bool s_wasEdit = false;
        bool s_appliedOverride = false;
        int  s_appliedType = -1;
        float s_appliedAlpha = -1.0f;
        float s_alphaBeforeOverride = 1.0f;
        bool s_haveSavedAlpha = false;

        bool s_preloading = false;
        int  s_preloadType = -1;
        ULONGLONG s_loadAfterMs = 0;

        // Avoid retrying a missing scripting-gta export on every Scaleform call.
        ULONGLONG s_nextCfxBindTryMs = 0;

        std::uint64_t packFloat(float v)
        {
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
            std::memcpy(&bits, &v, sizeof(bits));
            return (std::uint64_t)bits;
        }

        float unpackFloat(std::uint64_t raw, float fallback)
        {
            const std::uint32_t bits = (std::uint32_t)(raw & 0xFFFFFFFFu);
            float v = fallback;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }

        FARPROC findExport(HMODULE module, const char* plain, const char* decorated)
        {
            if (!module) return nullptr;
            if (plain)
                if (FARPROC p = GetProcAddress(module, plain)) return p;
            return decorated ? GetProcAddress(module, decorated) : nullptr;
        }

        // Find a C++ export without hardcoding MSVC's mangled spelling. FiveM
        // exports ScriptEngine as C++, so the exact decorated name can change
        // with compiler details while the readable method name stays present.
        FARPROC findExportContaining(HMODULE module, const char* needle)
        {
            if (!module || !needle || !*needle) return nullptr;

            auto* base = reinterpret_cast<unsigned char*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!dir.VirtualAddress || !dir.Size) return nullptr;

            auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
            auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);

            for (DWORD i = 0; i < exp->NumberOfNames; ++i)
            {
                const char* name = reinterpret_cast<const char*>(base + names[i]);
                if (name && std::strstr(name, needle))
                    return GetProcAddress(module, name);
            }
            return nullptr;
        }

        bool invokeCfx(std::uint64_t hash, const std::uint64_t* args, int nargs,
                       std::uint64_t* result)
        {
            if (!s_cfxCallNative) return false;
            if (nargs < 0 || nargs > 32) return false;

            alignas(16) std::uint64_t data[32]{};
            for (int i = 0; i < nargs; ++i) data[i] = args[i];

            CfxScriptContext ctx{};
            ctx.argumentBuffer = data;
            ctx.resultBuffer = data;
            ctx.numArguments = nargs;
            ctx.numResults = 0;

            bool ok = false;
            __try
            {
                ok = s_cfxCallNative(hash, ctx);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                logger::write("info", "cloudhat: FiveM direct native 0x%llX faulted",
                    (unsigned long long)hash);
                ok = false;
            }

            if (ok && result) *result = data[0];
            return ok;
        }

        bool invokeShv(std::uint64_t hash, const std::uint64_t* args, int nargs,
                       std::uint64_t* result)
        {
            if (!s_shvBound) return false;

            s_nativeInit(hash);
            for (int i = 0; i < nargs; ++i) s_nativePush64(args[i]);
            std::uint64_t* out = s_nativeCall();
            if (result && out) *result = *out;

            // ScriptHookV has no explicit success return. On FiveM V2 this path
            // is only fallback; the direct Cfx path above is preferred.
            return true;
        }

        bool invoke(std::uint64_t hash, const std::uint64_t* args, int nargs,
                    std::uint64_t* result = nullptr)
        {
            if (s_cfxCallNative && invokeCfx(hash, args, nargs, result)) return true;
            return invokeShv(hash, args, nargs, result);
        }

        bool invoke0(std::uint64_t hash)
        {
            return invoke(hash, nullptr, 0, nullptr);
        }

        bool invokeString(std::uint64_t hash, const char* text)
        {
            const std::uint64_t args[] = {
                (std::uint64_t)(std::uintptr_t)text,
            };
            return invoke(hash, args, 1, nullptr);
        }

        bool invokeStringFloat(std::uint64_t hash, const char* text, float value)
        {
            const std::uint64_t args[] = {
                (std::uint64_t)(std::uintptr_t)text,
                packFloat(value),
            };
            return invoke(hash, args, 2, nullptr);
        }

        bool invokeFloat(std::uint64_t hash, float value)
        {
            const std::uint64_t args[] = { packFloat(value) };
            return invoke(hash, args, 1, nullptr);
        }

        float invokeFloatResult(std::uint64_t hash, float fallback)
        {
            std::uint64_t raw = 0;
            return invoke(hash, nullptr, 0, &raw) ? unpackFloat(raw, fallback) : fallback;
        }

        void cancelPreload()
        {
            s_preloading = false;
            s_preloadType = -1;
            s_loadAfterMs = 0;
        }

        void clearAppliedState(bool restoreAlpha)
        {
            if (s_appliedOverride || s_preloading)
            {
                invoke0(H_UNLOAD_ALL_CLOUD_HATS);
                if (restoreAlpha && s_haveSavedAlpha)
                    invokeFloat(H_SET_CLOUDS_ALPHA, s_alphaBeforeOverride);
            }

            s_appliedOverride = false;
            s_appliedType = -1;
            s_appliedAlpha = -1.0f;
            s_haveSavedAlpha = false;
            cancelPreload();
        }

        void ensureBackend()
        {
            if (s_cfxCallNative || s_shvBound) return;

            const ULONGLONG now = GetTickCount64();
            if (now < s_nextCfxBindTryMs) return;
            s_nextCfxBindTryMs = now + 1000;

            bindCfx();
        }
    }

    bool bindCfx()
    {
        if (s_cfxCallNative) return true;

        HMODULE cfx = GetModuleHandleA("scripting-gta.dll");
        if (!cfx) return false;

        FARPROC p = findExportContaining(cfx, "CallNativeHandler@ScriptEngine@fx");
        if (!p) p = findExportContaining(cfx, "CallNativeHandler");

        s_cfxCallNative = reinterpret_cast<CfxCallNativeFn>(p);
        if (s_cfxCallNative)
        {
            logger::write("info",
                "cloudhat: FiveM scripting-gta direct native bridge ready (Editor-safe V2)");
            return true;
        }

        if (!s_cfxBindLogged)
        {
            s_cfxBindLogged = true;
            logger::write("info",
                "cloudhat: scripting-gta present but CallNativeHandler export was not found");
        }
        return false;
    }

    bool bindScriptHook(HMODULE shv)
    {
        if (s_shvBound) return true;
        if (!shv) return false;

        s_nativeInit = reinterpret_cast<NativeInitFn>(findExport(
            shv, "nativeInit", "?nativeInit@@YAX_K@Z"));
        s_nativePush64 = reinterpret_cast<NativePush64Fn>(findExport(
            shv, "nativePush64", "?nativePush64@@YAX_K@Z"));
        s_nativeCall = reinterpret_cast<NativeCallFn>(findExport(
            shv, "nativeCall", "?nativeCall@@YAPEA_KXZ"));

        s_shvBound = s_nativeInit && s_nativePush64 && s_nativeCall;
        if (s_shvBound)
            logger::write("info", "cloudhat: ScriptHookV native bridge ready (fallback)");
        return s_shvBound;
    }

    bool ready()
    {
        return s_cfxCallNative || s_shvBound;
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
        ensureBackend();
        if (!ready()) return;

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

        if (!want)
        {
            if (s_appliedOverride || s_preloading)
                clearAppliedState(true);
            s_wasEdit = true;
            return;
        }

        // Save the clip/editor alpha only once when the override begins.
        if ((!s_wasEdit || (!s_appliedOverride && !s_preloading)) && !s_haveSavedAlpha)
        {
            s_alphaBeforeOverride = invokeFloatResult(H_GET_CLOUDS_ALPHA, 1.0f);
            s_haveSavedAlpha = true;
        }

        // A new selection starts a tiny PRELOAD -> LOAD state machine. The
        // native documentation explicitly separates these operations. Waiting
        // ~50 ms gives the streamer a chance to make the named hat resident,
        // while still feeling instant in the editor UI.
        if ((!s_appliedOverride || s_appliedType != type) &&
            (!s_preloading || s_preloadType != type))
        {
            if (s_appliedOverride || s_preloading)
                invoke0(H_UNLOAD_ALL_CLOUD_HATS);

            cancelPreload();
            if (invokeString(H_PRELOAD_CLOUD_HAT, typeName(type)))
            {
                s_preloading = true;
                s_preloadType = type;
                s_loadAfterMs = GetTickCount64() + 50;
                s_appliedOverride = false;
                s_appliedType = -1;
                logger::write("info", "cloudhat: preloading '%s'", typeName(type));
            }
            else
            {
                logger::write("info", "cloudhat: PRELOAD failed for '%s'", typeName(type));
            }
        }

        if (s_preloading && s_preloadType == type && GetTickCount64() >= s_loadAfterMs)
        {
            if (invokeStringFloat(H_LOAD_CLOUD_HAT, typeName(type), 0.0f))
            {
                s_appliedType = type;
                s_appliedOverride = true;
                cancelPreload();
                // Force alpha application after the new hat is actually loaded.
                s_appliedAlpha = -1.0f;
                logger::write("info", "cloudhat: loaded '%s' for Rockstar Editor playback",
                    typeName(type));
            }
            else
            {
                logger::write("info", "cloudhat: LOAD failed for '%s'", typeName(type));
                cancelPreload();
            }
        }

        if (s_appliedOverride &&
            (s_appliedAlpha < 0.0f || fabsf(s_appliedAlpha - alpha) > 0.0001f))
        {
            if (invokeFloat(H_SET_CLOUDS_ALPHA, alpha))
                s_appliedAlpha = alpha;
        }

        s_wasEdit = true;
    }
}
