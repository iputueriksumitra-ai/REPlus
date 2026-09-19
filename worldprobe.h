// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>

// =============================================================================
//  One line probe against the world
// =============================================================================
//  Autofocus needs to know what is actually in front of the lens, and there is
//  no other way to ask. An ASI cannot read the depth buffer, and the value
//  would be wrong even if it could: a session samples the frame after the lens
//  has already been displaced, so the depth under a screen point is not the
//  depth of the thing the shot is composed around. The engine's own query is
//  the only answer that stays true while the camera moves.
//
//  Deliberately NOT a capsule. The free camera's collision sweep uses one,
//  because it is asking "can I fit here"; focus is asking "what is exactly
//  there", and a capsule answers with whatever its shell brushes first - which
//  near a subject's edge is the wall behind it.
//
//  Deliberately NOT the sweep's include mask either. That is map geometry only,
//  no peds and no vehicles, so borrowing it would focus straight through the
//  subject. See signatures.h.
// =============================================================================
namespace worldprobe
{
	// Everything resolved and the probe is usable.
	bool available();

	struct Hit
	{
		bool  hit   = false;
		float t     = 1.0f;   // fraction along start..end
		float x = 0.0f, y = 0.0f, z = 0.0f;

		// Diagnostics. A miss and a wrong distance look identical from the
		// outside, and telling them apart is the whole difficulty here:
		// submitted=false means the engine refused the descriptor, count=0 means
		// it ran and found nothing, and count>0 with no usable t means the
		// entries are not what we think they are.
		bool     submitted = false;
		uint32_t count     = 0;
		float    firstT    = -1.0f;
	};

	// Fire from start to end. Returns false if unavailable or nothing was hit,
	// in which case `out` is left at t = 1 and the ray's own end point.
	//
	// Main thread only: the test takes the engine's own lock and is submitted
	// synchronously, so it must run where the world is not being stepped.
	bool line(const float start[3], const float end[3], Hit* out);
}
