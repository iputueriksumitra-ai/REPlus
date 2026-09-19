// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace menu
{
	// Installs the Video Editor camera-menu row injection. Safe to call when
	// the menu addresses did not resolve — it simply does nothing and the
	// spline keeps working off the ini alone.
	void install();

	// Once per frame, on the game's main thread.
	//
	// Only the add-on's "Copy to keyframe" button so far: it bumps a counter in
	// the shared block and this is what notices. It has to be polled from
	// outside the menu's input handler, because the press happens in ReShade's
	// overlay - the editor never sees an input at all.
	void tick();

	// The marker the editor currently has OPEN - what the per-marker rows edit.
	// Null outside the editor, or when the addresses behind it did not resolve.
	//
	// Exposed because the scene-light track has to key against the same marker
	// the menu does. The DIRECTOR's current marker is a different thing: it
	// follows the playhead, so selecting a marker in the list moves this one and
	// not that one, and a track keyed on the director's would keep filing edits
	// against whichever marker was open before.
	void* currentEditMarker();
}
