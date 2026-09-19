// This file is part of RockstarEditorPlus.
// WOW TEAM SCENE extension: Cloud Hat override for Rockstar Editor playback.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <Windows.h>

namespace cloudhat
{
    // FiveM V2 preferred path. Resolve scripting-gta.dll's exported
    // fx::ScriptEngine::CallNativeHandler and invoke the game native directly.
    // This deliberately avoids FiveM ScriptHookV's networkInited gate, which is
    // not reliable while Rockstar Editor owns the session.
    bool bindCfx();

    // Fallback path for ordinary ScriptHookV / non-FiveM hosts. Safe to call
    // repeatedly; FiveM will still prefer the Cfx bridge when available.
    bool bindScriptHook(HMODULE shv);

    // True once either native bridge is available.
    bool ready();

    // State-driven heartbeat, called from RE+'s own Scaleform/editor update
    // path. Native calls happen only on enter/leave, selection changes, and
    // opacity changes. A short preload->load state is used for new hats.
    void tick();

    int typeCount();
    const char* typeName(int index);
}
