// This file is part of RockstarEditorPlus.
// SPDX-License-Identifier: GPL-3.0-only
//
// WOW TEAM SCENE V4
// Suppresses the Rockstar Editor render-identification overlay that FiveM can
// still draw into the back buffer even when RE+ bypasses Rockstar's encoder.
// The patch is deliberately narrow: only the known Editor watermark renderer
// is returned early. FiveM branding and all other HUD/UI drawing are untouched.

#include "main.h"
#include "replay/editorwatermark.h"

#include <vector>
#include <cstdlib>
#include <cstring>

namespace editorwatermark
{
    namespace
    {
        bool s_ready = false;

        // Signature of the Rockstar Editor render-watermark renderer.
        // Kept local so a future build mismatch disables only this feature.
        constexpr const char* kRendererPattern =
            "48 83 EC ? 8B 0D ? ? ? ? 65 48 8B 04 25 ? ? ? ? BA ? ? ? ? "
            "48 8B 04 C8 8B 0C 02 D1 E9 F6 C1 ? 74 ? 83 3D";

        std::vector<int> patternBytes(const char* pattern)
        {
            std::vector<int> out;
            const char* cur = pattern;
            const char* end = pattern + std::strlen(pattern);

            while (cur < end)
            {
                if (*cur == ' ')
                {
                    ++cur;
                    continue;
                }
                if (*cur == '?')
                {
                    ++cur;
                    if (cur < end && *cur == '?') ++cur;
                    out.push_back(-1);
                    continue;
                }

                char* next = nullptr;
                const unsigned long value = std::strtoul(cur, &next, 16);
                if (next == cur)
                {
                    ++cur;
                    continue;
                }
                out.push_back((int)(value & 0xFFu));
                cur = next;
            }
            return out;
        }

        uintptr_t findUnique(const char* pattern, int& count)
        {
            count = 0;
            const uintptr_t base = memory::base();
            const uintptr_t size = memory::imageSize();
            if (!base || !size) return 0;

            const std::vector<int> sig = patternBytes(pattern);
            if (sig.empty() || size < sig.size()) return 0;

            const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
            uintptr_t first = 0;

            for (uintptr_t i = 0; i <= size - sig.size(); ++i)
            {
                bool match = true;
                for (size_t j = 0; j < sig.size(); ++j)
                {
                    if (sig[j] >= 0 && bytes[i + j] != (std::uint8_t)sig[j])
                    {
                        match = false;
                        break;
                    }
                }

                if (!match) continue;
                if (++count == 1) first = base + i;
                if (count > 1) return 0; // refuse an ambiguous patch
            }
            return count == 1 ? first : 0;
        }
    }

    bool ready()
    {
        return s_ready;
    }

    void install()
    {
        if (s_ready) return;

        int matches = 0;
        const uintptr_t renderer = findUnique(kRendererPattern, matches);
        if (!renderer)
        {
            logger::write("info",
                "editor watermark: renderer signature %s (%d match%s) - left unchanged",
                matches == 0 ? "not found" : "ambiguous",
                matches, matches == 1 ? "" : "es");
            return;
        }

        // The renderer is a leaf from our point of view. Returning immediately
        // prevents only its overlay from being submitted; the editor/playback
        // and RE+ render state machine keep running normally.
        const std::uint8_t was = *reinterpret_cast<const std::uint8_t*>(renderer);
        if (was != 0xC3)
            memory(renderer, false).put<std::uint8_t>(0xC3);

        s_ready = true;
        logger::write("info", "editor watermark: disabled at %p", (void*)renderer);
    }
}
