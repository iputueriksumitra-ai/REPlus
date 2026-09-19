// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Scene lights — free-standing point and spot lights, placed anywhere
// =============================================================================
//  Not attached to a prop, an entity or a ymap. The engine's scene light list
//  is immediate-mode: its count is zeroed every frame after the renderer has
//  consumed it, so a light exists for exactly the frame it was added. We
//  therefore re-add the whole set every frame, which also means editing is
//  instant and there is nothing to clean up on shutdown.
//
//  The list works the same way on both builds — allocate a slot, fill it in
//  place:
//
//      CLightSource* L = AddSceneLight();
//      if (!L) return;
//      lightbuild::BuildLight(L, params);
//
//  Enhanced additionally has a parallel job-based light gather, which was the
//  first thing this was built on. That turned out to be the wrong route: the
//  gather is not dispatched at all when a view has no source lights, which
//  measured as over half of a real session, so any light we injected there
//  vanished whenever the camera looked somewhere dark. The plain list above
//  exists on both builds and its consumer runs every frame regardless, so that
//  is what this uses.
//
// =============================================================================
//  Why the struct is built by hand
// =============================================================================
//  Enhanced inlined CLightSource's constructor — its light producers call no
//  SetCommon at all, so there is no function to call. Same problem worldprobe
//  hit with the shape-test descriptor, and the same solution: read what the
//  ctor writes off the Legacy copy (which is not inlined) and replicate it.
//  The struct is byte-identical between builds, so one builder serves both.
//  See lightbuild.h.

namespace lights
{
	// Resolve the light API and install the per-frame hook. Safe to call when
	// the addresses did not resolve; the feature simply stays off.
	void install();

	// True once the hook is in place.
	bool ready();

	// Number of lights currently enabled in the set.
	int activeCount();

	// Where the set is stored. Empty until install() has run.
	const char* iniPath();

	// Push edits to the render side and persist one light. The menu calls this
	// after every change, so the light on screen and the file on disk never
	// disagree with what the rows say.
	void commit(int index);

	// Same, for a change that altered the SET rather than one light - an add or
	// a delete. The ini path has to rewrite the whole file (a per-light write
	// leaves the deleted light's section behind), and the store has to be told
	// so the change reaches every keyframe rather than just the current one.
	void commitAll();

	// Position of the editor camera, and the point it is looking at `dist`
	// metres ahead. Used by the menu's placement rows so a light can be put
	// where you are looking rather than typed in as coordinates.
	// False when there is no camera to read (outside the editor).
	bool cameraPos(float outPos[3]);
	bool cameraAim(float outDir[3]);

	// -------------------------------------------------------------------------
	//  Driving a light with the camera
	// -------------------------------------------------------------------------
	//  Grabbing a light hands it to the free camera: it sits where the camera
	//  sits and points where the camera points, every frame, until it is
	//  released. Fly to where you want it, drop it, carry on.
	//
	//  This is the only way to MOVE a light after placing it - the menu has rows
	//  for what a light looks like but none for where it is, and typing
	//  coordinates into a file is not editing. It is the same swap-mode idea the
	//  editor's own Camera Type row uses: change what the stick is driving, move
	//  it, change back.

	// Grab `index`, or pass -1 to release. Releasing does not move anything -
	// the light stays exactly where the camera left it.
	void grab(int index);

	// Which light the camera is driving, or -1.
	int grabbed();

	// Per-frame, MAIN THREAD. Copies the camera onto the grabbed light and
	// publishes. Cheap and silent when nothing is grabbed.
	void tickGrab();
}
