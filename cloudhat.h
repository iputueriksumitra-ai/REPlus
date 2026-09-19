// This file is part of RockstarEditorPlus.
// WOW TEAM SCENE extension: Cloud Hat override for Rockstar Editor playback.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <Windows.h>

namespace cloudhat
{
    // Resolve ScriptHookV's native-call exports. Safe to call repeatedly.
    // The bridge is intentionally dynamic so the stock RE+ build remains
    // self-contained and does not acquire a new import-library dependency.
    bool bindScriptHook(HMODULE shv);

    // True once the native bridge is available. Under FiveM this becomes true
    // when its ScriptHookV compatibility DLL is ready and the RE+ script fiber
    // is registered.
    bool ready();

    // Called from RE+'s ScriptHookV fiber. The function itself runs every frame,
    // but native calls are EDGE/STATE driven: it only loads/unloads a hat when
    // entering/leaving the editor or when the selected hat changes, and only
    // writes alpha when the requested opacity changes.
    void tick();

    int typeCount();
    const char* typeName(int index);
}
