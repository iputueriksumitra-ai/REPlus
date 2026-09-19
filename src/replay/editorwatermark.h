// This file is part of RockstarEditorPlus.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace editorwatermark
{
    // Suppress the Rockstar Editor render-identification overlay (star icon,
    // render code and project/clip label). Safe no-op when the known renderer
    // signature is not present or is not unique on the running build.
    void install();
    bool ready();
}
