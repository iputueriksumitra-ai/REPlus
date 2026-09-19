// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "lights/lights.h"
#include "lights/lightbuild.h"
#include "lights/lightmodel.h"
#include "lights/lightstore.h"
#include "game/game.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "replay/marker.h"
#include "replay/freecam.h"
#include "replay/smoothblend.h"

#include <atomic>

namespace lights
{
	namespace
	{
		using AddSceneLightFn = void* (*)();
		using ConsumerFn = void(*)();

		AddSceneLightFn g_addSceneLight = nullptr;
		ConsumerFn      origConsumer    = nullptr;

		std::atomic<bool> g_ready{false};
		std::atomic<int>  g_active{0};

		std::string g_iniPath;

		// Which light the camera is driving, or -1. Main thread only: every
		// reader and writer is either the menu or the per-frame tick, both of
		// which run there.
		int g_grab = -1;

		// Where the camera was when the grab started, so releasing can put it
		// back. Without this the feature is only half of what it should be: you
		// fly off to place a light and your shot is wherever you left it.
		struct CamSave
		{
			bool  valid = false;
			float a[3]{}, b[3]{}, c[3]{};   // frame basis
			float framePos[3]{};
			float rootPos[3]{};             // OFF_Position, the authoritative one
		};
		CamSave g_camSave;

		// World -> attach space.
		//
		// The attach matrix is orthonormal (validMatrix insists), so the inverse
		// is the transpose plus a translation rather than a general inverse.
		spline::Vec3 untransform(const rfreecam::Mat34& m, const spline::Vec3& w)
		{
			const spline::Vec3 r = w - m.d;
			return spline::Vec3(r.x * m.a.x + r.y * m.a.y + r.z * m.a.z,
			                    r.x * m.b.x + r.y * m.b.y + r.z * m.b.z,
			                    r.x * m.c.x + r.y * m.c.y + r.z * m.c.z);
		}

		// The camera the editor is actually flying.
		//
		// The director's two FREE-CAMERA slots, not director+ActiveCamera. The
		// active camera is only the free camera some of the time: set a marker's
		// Blend Mode and it becomes the blend camera instead, a different class
		// where +0x480 is not a position at all. Writing there did nothing
		// useful, which is why the grab quietly stopped teleporting the moment
		// any of the Cameras rows were set.
		//
		// Guarded by an orthonormality test on the basis, exactly as freecam.h
		// argues: this is a check on OUR offsets, not on the game's data, and
		// getting them wrong here does not mean a slightly-off camera - it means
		// writing a garbage transform into the thing the user is looking through.
		void* freeCam()
		{
			void* d = smoothblend::lastDirector();
			if (!d) return nullptr;

			void* cam = rfreecam::forMarker(d, rdirector::currentMarkerIndex(d));
			if (!cam)
			{
				// The director rebuilds both cameras when the playhead crosses a
				// marker, and for a few frames afterwards neither carries the
				// index being asked about. Take whichever slot holds a camera -
				// the validity test below is what actually decides.
				for (uint32_t off : { (uint32_t)rfreecam::DIR_FreeCamera0,
				                      (uint32_t)rfreecam::DIR_FreeCamera1 })
				{
					if (void* c = *(void**)((uint8_t*)d + off)) { cam = c; break; }
				}
			}
			if (!cam) return nullptr;

			const uint8_t* p = (const uint8_t*)cam;
			rfreecam::Mat34 m;
			m.a = spline::Vec3((const float*)(p + rfreecam::OFF_FrameMatrixA));
			m.b = spline::Vec3((const float*)(p + rfreecam::OFF_FrameMatrixB));
			m.c = spline::Vec3((const float*)(p + rfreecam::OFF_FrameMatrixC));
			m.d = spline::Vec3((const float*)(p + rfreecam::OFF_FramePosition));
			if (!rfreecam::validMatrix(m)) return nullptr;

			// An attached camera ("Move with Target") is fine - writeCam brings
			// the world position into attach space. It is only refused if the
			// attach matrix itself does not look like a rotation.
			if (rfreecam::attachEntity(cam) &&
			    !rfreecam::validMatrix(rfreecam::attachMatrix(cam)))
				return nullptr;

			return cam;
		}

		void readCam(void* cam, CamSave& s)
		{
			const uint8_t* p = (const uint8_t*)cam;
			auto rd = [&](uint32_t off, float* out) {
				memcpy(out, p + off, sizeof(float) * 3);
			};
			rd(rfreecam::OFF_FrameMatrixA,  s.a);
			rd(rfreecam::OFF_FrameMatrixB,  s.b);
			rd(rfreecam::OFF_FrameMatrixC,  s.c);
			rd(rfreecam::OFF_FramePosition, s.framePos);
			rd(rfreecam::OFF_Position,      s.rootPos);
			s.valid = true;
		}

		// Put the camera somewhere. `basis` may be null to leave the orientation
		// alone, which is what a point light wants - it has no direction worth
		// facing, and spinning the view for no reason is just disorienting.
		void writeCam(void* cam, const float pos[3], const rfreecam::Mat34* basis)
		{
			uint8_t* p = (uint8_t*)cam;
			const spline::Vec3 world(pos[0], pos[1], pos[2]);

			// OFF_Position is in ATTACH space whenever the camera is mounted on
			// something, so a world position has to be brought into that frame
			// first. The frame position stays world either way - it is the
			// rendered result, not the input.
			spline::Vec3 root = world;
			if (rfreecam::attachEntity(cam))
				root = untransform(rfreecam::attachMatrix(cam), world);

			// Both, and in this order. OFF_Position is what the next update
			// builds from; the frame is what THIS frame still renders, so
			// setting only the former shows one stale frame before it catches up.
			root.store((float*)(p + rfreecam::OFF_Position));
			world.store((float*)(p + rfreecam::OFF_FramePosition));

			if (!basis) return;
			basis->a.store((float*)(p + rfreecam::OFF_FrameMatrixA));
			basis->b.store((float*)(p + rfreecam::OFF_FrameMatrixB));
			basis->c.store((float*)(p + rfreecam::OFF_FrameMatrixC));
		}

		// Put back exactly what readCam saw. Raw, with no attach conversion:
		// rootPos was saved in whatever space the camera keeps it in, so
		// converting on the way back would apply the transform a second time.
		void restoreCam(void* cam, const CamSave& s)
		{
			uint8_t* p = (uint8_t*)cam;
			memcpy(p + rfreecam::OFF_Position,      s.rootPos,  sizeof(float) * 3);
			memcpy(p + rfreecam::OFF_FramePosition, s.framePos, sizeof(float) * 3);
			memcpy(p + rfreecam::OFF_FrameMatrixA,  s.a,        sizeof(float) * 3);
			memcpy(p + rfreecam::OFF_FrameMatrixB,  s.b,        sizeof(float) * 3);
			memcpy(p + rfreecam::OFF_FrameMatrixC,  s.c,        sizeof(float) * 3);
		}

		// Live count of the scene light list, straight from the descriptor.
		// Diagnostic only, but a useful one: non-zero here means the light
		// system is running in whatever context we are in.
		unsigned gameLightCount()
		{
			if (!game::addr_g_SceneLights) return 0;
			unsigned short n = 0;
			memcpy(&n, (const void*)(game::addr_g_SceneLights + 8), sizeof(n));
			return n;
		}

		// Runs immediately before the game consumes the light list, so anything
		// added here is rendered this frame. That ordering is why the consumer
		// is the hook point rather than a camera tick — it is correct by
		// construction rather than by argument.
		void hkConsumer()
		{
			const lightmodel::Snapshot& set = lightmodel::Current();

			int on = 0;
			for (int i = 0; i < set.count; ++i)
			{
				if (!set.lights[i].enabled) continue;

				void* slot = g_addSceneLight();
				if (!slot)
				{
					// The array grows on demand, so this means a real allocation
					// failure. Stop rather than spin on it.
					break;
				}
				// `i` is the light's stable index in the set, which is exactly
				// what the shadow identity needs: distinct per light, and the
				// same every frame so the shadow manager can track it.
				lightbuild::BuildLight((unsigned char*)slot, set.lights[i].p,
				                       (uint32_t)i);
				++on;
			}
			g_active.store(on, std::memory_order_relaxed);

			origConsumer();
		}

		// The set is re-read off disk so lights can be added, moved and retuned
		// without a restart. On its own thread because the only per-frame hooks
		// available are on the render path, and file I/O does not belong there.
		//
		// Stands down while the per-clip store owns the set. Without that, two
		// seconds after any edit inside the editor this would reload the ini
		// over the top of it - the per-clip lights would appear to work and then
		// silently revert, which is a far worse failure than not reloading.
		DWORD WINAPI reloadThread(LPVOID)
		{
			for (;;)
			{
				Sleep(2000);
				if (lightstore::scoped()) continue;
				lightmodel::Load(g_iniPath.c_str());
			}
		}
	}

	const char* iniPath() { return g_iniPath.c_str(); }

	// Whoever owns the set takes the write.
	//
	// Inside the editor that is the per-clip store, and the ini is deliberately
	// left alone: it is the DEFAULT set every unlit clip starts from, so letting
	// per-clip edits leak into it would quietly rewrite the starting point for
	// every other clip. Outside the editor the ini is the set, and is written
	// directly as before.
	void commit(int index)
	{
		lightmodel::Publish();

		if (lightstore::scoped())
		{
			lightstore::noteEdit();   // flushed by lightstore::tick()
			return;
		}

		if (!g_iniPath.empty())
		{
			lightmodel::SaveCount(g_iniPath.c_str());
			lightmodel::SaveLight(g_iniPath.c_str(), index);
		}
	}

	void commitAll()
	{
		lightmodel::Publish();

		if (lightstore::scoped())
		{
			lightstore::noteEdit();
			return;
		}

		if (!g_iniPath.empty())
			lightmodel::SaveAll(g_iniPath.c_str());
	}

	// The director's frame matrix is the editor camera. Reading it here keeps
	// the menu free of engine offsets - it asks for a position, not a struct.
	bool cameraPos(float out[3])
	{
		void* d = smoothblend::lastDirector();
		if (!d) return false;
		const float* p = (const float*)((unsigned char*)d + rdirector::OFF_FramePosition);
		out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
		return true;
	}

	bool cameraAim(float out[3])
	{
		void* d = smoothblend::lastDirector();
		if (!d) return false;
		const float* f = (const float*)((unsigned char*)d + rdirector::OFF_FrameMatrixB);
		out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
		return true;
	}

	int grabbed() { return g_grab; }

	void grab(int index)
	{
		if (index == g_grab) return;

		void* cam = freeCam();

		// ---- releasing -------------------------------------------------------
		if (g_grab >= 0)
		{
			// Persists once, here, rather than on every frame of the move:
			// tickGrab would otherwise re-arm the store's debounce every frame
			// and nothing would reach disk until you let go anyway.
			commit(g_grab);

			// Hand the shot back. This is the half that makes the feature worth
			// having - you flew off to place a light, and the camera returns to
			// the frame you set up rather than staying wherever the light ended.
			if (cam && g_camSave.valid) restoreCam(cam, g_camSave);
			g_camSave.valid = false;
		}

		g_grab = index;
		if (g_grab < 0) { lightmodel::Publish(); return; }

		// ---- grabbing --------------------------------------------------------
		if (g_grab >= lightmodel::Count()) { g_grab = -1; return; }

		// A light you cannot see is a light you cannot aim. Turning it on is the
		// only sane reading of "grab this one", and leaving it off would look
		// exactly like the grab not working.
		lightmodel::Mutable(g_grab).enabled = true;

		const lightbuild::LightParams& p = lightmodel::Mutable(g_grab).p;

		if (cam)
		{
			readCam(cam, g_camSave);

			// Face down the beam for a spot - that is the whole point, you are
			// looking at what it lights. A point light has no direction worth
			// facing, so keep the orientation and only move.
			if (p.type == lightbuild::kSpot)
			{
				rfreecam::Mat34 m;
				lookat::worldMatrixFromFront(
					spline::Vec3(p.dir[0], p.dir[1], p.dir[2]), m);
				writeCam(cam, p.pos, &m);
			}
			else
			{
				writeCam(cam, p.pos, nullptr);
			}
		}
		else
		{
			// No camera to move, so the light snaps to wherever the camera is
			// instead. Worth saying out loud: it is the old behaviour, and the
			// reason is always one of the validity guards in freeCam().
			logger::write("info",
				"lights: grabbed light %d but no usable free camera was found - "
				"it will follow the camera without teleporting", g_grab);
		}

		lightmodel::Publish();
	}

	void tickGrab()
	{
		if (g_grab < 0) return;

		// The set can be swapped underneath us by a clip change, and Remove()
		// shuffles everything down. Neither is worth special-casing - dropping
		// the light is the safe answer, and it is what the user sees anyway.
		if (g_grab >= lightmodel::Count()) { g_grab = -1; return; }

		float pos[3], aim[3];
		if (!cameraPos(pos) || !cameraAim(aim)) return;

		lightbuild::LightParams& p = lightmodel::Mutable(g_grab).p;

		// Publishing copies the whole set and flips the buffer. Doing that every
		// frame while the camera is parked is pure waste, and a parked camera is
		// the normal state while you look at what you just placed.
		if (p.pos[0] == pos[0] && p.pos[1] == pos[1] && p.pos[2] == pos[2] &&
		    p.dir[0] == aim[0] && p.dir[1] == aim[1] && p.dir[2] == aim[2])
			return;

		p.pos[0] = pos[0]; p.pos[1] = pos[1]; p.pos[2] = pos[2];

		// Written for every type. A point light ignores the direction, and
		// branching on the type here would only mean the aim is stale the moment
		// the light is switched to a spot.
		p.dir[0] = aim[0]; p.dir[1] = aim[1]; p.dir[2] = aim[2];

		lightmodel::Publish();
	}

	bool ready() { return g_ready.load(std::memory_order_relaxed); }
	int  activeCount() { return g_active.load(std::memory_order_relaxed); }

	void install()
	{
		if (!game::addr_AddSceneLight || !game::addr_LightConsumer ||
		    !game::addr_g_SceneLights)
		{
			logger::write("info", "lights: not resolved - scene lights disabled");
			return;
		}

		// Beside the other two inis, not at the game root - paths::file() is what
		// RockstarEditorPlus.ini and Render.ini use, and the per-clip light track
		// already lands under the same folder via paths::sub("lights").
		g_iniPath = paths::file("Lights.ini");
		lightmodel::Load(g_iniPath.c_str());

		// Per-clip sets. The ini above stays the default every unlit clip starts
		// from, so it is loaded first and this layers on top of it.
		lightstore::load();

		g_addSceneLight = (AddSceneLightFn)game::addr_AddSceneLight;

		memory(game::addr_LightConsumer).hook(hkConsumer, &origConsumer);

		CreateThread(nullptr, 0, reloadThread, nullptr, 0, nullptr);

		g_ready.store(true, std::memory_order_relaxed);

		const lightmodel::Snapshot& set = lightmodel::Current();
		logger::write("info", "lights: installed - %d light(s) in %s",
			set.count, g_iniPath.c_str());
		for (int i = 0; i < set.count; ++i)
		{
			const lightmodel::Entry& e = set.lights[i];
			logger::write("info",
				"lights:   [%d] %-5s %s pos=(%.1f, %.1f, %.1f) range=%.1f "
				"int=%.1f flags=0x%08X%s",
				i, e.p.type == lightbuild::kSpot ? "spot" : "point",
				e.enabled ? "on " : "OFF", e.p.pos[0], e.p.pos[1], e.p.pos[2],
				e.p.range, e.p.intensity, e.p.flags,
				(e.p.flags & lightbuild::kCastShadows) ? "  [shadows]" : "");
		}
		logger::write("info", "lights: game's own light count right now = %u",
			gameLightCount());
	}
}
