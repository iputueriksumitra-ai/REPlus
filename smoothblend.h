// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace smoothblend
{
	// Installs the camReplayDirector::UpdateSmoothing detour. Safe to call when
	// nothing resolved — it simply does nothing.
	void install();

	// The camReplayDirector instance seen by the most recent hook call, or null
	// before playback has run. We are handed it every frame anyway, so the UI
	// can read the live camera frame without resolving the director global.
	void* lastDirector();

	// The IGCS depth-of-field focus for THIS frame, resolved per marker.
	//
	// Answered from inside the spline update rather than recomputed by the
	// renderer: the marker window, the knot times and the pchip are already
	// built there, and duplicating that lookup is how the two would drift.
	//
	// `delta` is interpolated across markers, so two values over a shot give a
	// focus pull. `autofocus` is not - a boolean has nothing to interpolate, so
	// it takes the marker being played.
	//
	// Returns false when nothing has been resolved yet (no clip, no markers), in
	// which case the caller should fall back to its own configured values.
	bool focusNow(float* delta, bool* autofocus);
}
