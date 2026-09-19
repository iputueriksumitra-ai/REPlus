// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Per-clip scene lights, keyframed to markers
// =============================================================================
//  A light set belongs to a scene, and a scene is a clip - so the set follows
//  (project, clip). Within a clip it is a TRACK: one keyframe per marker, and
//  the values between them are interpolated, the same way the camera is.
//
//  Point a spot down on one marker and up on the next and it sweeps between
//  them over the gap. Two markers side by side gives a snap, because the gap is
//  what the interpolation runs across.
//
//  WHERE IT LIVES. One file, lights\scenelights.txt, with the project name,
//  clip index and marker time as CONTENT rather than as a filename:
//
//      RockstarEditorPlusLights v2
//      project Project 20
//      clip 0
//      marker 0
//      light on type=spot x=-2119.7 y=-306.1 z=13.7 dirz=-1 ... flags=0x41C0
//      marker 3500
//      light on type=spot x=-2119.7 y=-306.1 z=13.7 dirz=1 ...
//      clip 1
//      marker 0
//      light on type=point ...
//
//  Per-project FILES would have been the obvious mirror of rsettings, but the
//  filename is where all that module's hard problems came from: UTF-8 names
//  reinterpreted in the system codepage, reserved device names, trailing dots
//  Windows silently drops, and a sanitiser that mapped two different names onto
//  one file. Keeping the name inside the file makes every one of those go away
//  instead of being solved a second time.
//
//  MARKER TIME IS THE KEY, for the reasons rsettings gives: the editor refuses
//  two markers at the same time within a clip, so it is unique there, and
//  unlike an index it survives markers being added or removed earlier on.
//
//  WHAT INTERPOLATES: position, direction, colour, intensity, range, falloff
//  and the cone angles. What does NOT: type, flags and enabled, which are taken
//  from the keyframe on the left. Half a point light is not a thing, and a
//  shadow flag cannot be 40% set.
//
//  A clip with ONE keyframe is a constant light set - which is exactly what
//  every clip authored before this existed becomes, and why v1 files still load.
//
//  RELATIONSHIP TO THE INI. RockstarEditorPlusLights.ini is the DEFAULT set: it
//  is what you get in free roam, and what a clip you have never lit starts
//  from. Inside the editor the store owns the model and the ini's reload thread
//  stands down, so per-clip work cannot be stomped by the file watcher two
//  seconds later. Leaving the editor hands the lights back to the ini set.
//
//  Every entry point below runs on the MAIN THREAD, matching lightmodel's
//  editing API. scoped() is the one exception - the ini reload thread polls it -
//  so it alone is atomic.
// =============================================================================
namespace lightstore
{
	// Read the file. Call once, at install.
	void load();

	// Per-frame. Notices the open project, clip or MARKER changing and swaps the
	// editable set to match, filing the outgoing keyframe first.
	void syncScope();

	// Per-frame, after syncScope. Evaluates the track at the current replay time
	// and pushes the result to the render side. Does nothing when the clip has a
	// single keyframe (there is nothing to animate) or while a light is being
	// driven by the camera (you are authoring, not playing back).
	void animate();

	// Per-frame. Flushes once the store has been quiet for ~0.5s, so holding a
	// value down costs one write instead of one per input repeat.
	void tick();

	// Flush now, if dirty. Rewrites the whole file.
	void save();

	// True while a project and clip are bound - i.e. the store, not the ini,
	// owns the light set. Read from the ini reload thread.
	bool scoped();

	// The set on screen changed. Files it under the current scope and starts
	// the debounce. No-op when unscoped, so the ini path stays in charge there.
	void noteEdit();

	// A light was added or removed. These are STRUCTURAL and have to be applied
	// to every keyframe, not just the one being edited.
	//
	// Light identity here is the index, so the index has to mean the same thing
	// at every keyframe or the track is nonsense. Filing a delete against only
	// the current keyframe leaves every other one still holding that light, and
	// the next light you add takes the freed index and inherits its animation -
	// which reads as a deleted light coming back from the dead, still moving.
	void lightAdded(int index);
	void lightRemoved(int index);

	// How many keyframes the current clip's track holds, and the marker time the
	// menu is editing. Both are for the menu's status row; -1 when unscoped.
	int  keyCount();
	long editingKey();
}
