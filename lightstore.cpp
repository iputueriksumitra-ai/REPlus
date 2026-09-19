// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "lights/lightstore.h"
#include "lights/lightmodel.h"
#include "lights/lightbuild.h"
#include "lights/lights.h"
#include "game/game.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "replay/marker.h"
#include "replay/smoothblend.h"
#include "ui/menu.h"

#include <map>
#include <vector>
#include <iterator>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lightstore
{
	namespace
	{
		using Set = lightmodel::Snapshot;

		// Marker time in whole milliseconds -> the light set at that marker.
		// Quantised for the same reason rsettings quantises: marker times are
		// floats but always whole ms, and a float-equality lookup would miss on
		// a 1-ulp difference between what we stored and what we read back.
		using Track = std::map<long, Set>;

		// project -> clip -> track. A clip present with an empty track is
		// meaningful ("I deleted the lights here") and is NOT the same as absent
		// ("never lit, start from the ini"), which is why the empty track has to
		// survive the round trip - see the `clip` line in load().
		std::map<std::string, std::map<int, Track>> g_store;

		// What a clip that has never been lit starts from: the ini set, captured
		// when the editor is first entered. Frozen for the session rather than
		// re-read, so every unlit clip in one sitting starts from the same place.
		Set g_defaults;

		std::string       g_project;
		int               g_clip = -1;
		long              g_key  = 0;      // marker time the menu is editing
		std::atomic<bool> g_scoped{false};

		bool          g_dirty   = false;
		unsigned long g_dirtyAt = 0;

		std::string g_path;

		constexpr const char* kHeader  = "RockstarEditorPlusLights v";
		constexpr int         kVersion = 2;

		// The flag and its timestamp are one piece of state: tick() debounces on
		// the timestamp, so a dirty flag raised without one never gets flushed.
		inline void markDirty()  { g_dirty = true;  g_dirtyAt = GetTickCount(); }
		inline void clearDirty() { g_dirty = false; g_dirtyAt = 0; }

		// strtod rather than atof, matching the ini reader: atof turns a typo
		// into 0.0, and for a coordinate that silently teleports the light to
		// the map origin instead of complaining.
		inline float F(const char* s) { return (float)strtod(s, nullptr); }

		// ---------------------------------------------------------------------
		//  Interpolation
		// ---------------------------------------------------------------------

		inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

		// Direction needs a real slerp, not a lerp.
		//
		// The motivating case is the one that breaks the cheap version: a spot
		// pointing straight down on one marker and straight up on the next. Those
		// are opposite vectors, so a normalised lerp passes through zero at the
		// midpoint and the direction is undefined exactly where the sweep should
		// be fastest.
		void slerpDir(const float a[3], const float b[3], float t, float out[3])
		{
			auto dot = [](const float* u, const float* v) {
				return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
			};
			auto norm = [](float* v) {
				const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
			};

			float d = dot(a, b);
			if (d > 1.0f) d = 1.0f;
			if (d < -1.0f) d = -1.0f;

			// Near-parallel: the arc is short enough that lerp and slerp agree to
			// well under a pixel, and the sin() below would divide by ~0.
			if (d > 0.9995f)
			{
				for (int i = 0; i < 3; ++i) out[i] = lerpf(a[i], b[i], t);
				norm(out);
				return;
			}

			// Antipodal: there is no unique arc between them, so pick an axis
			// perpendicular to `a` and rotate about it. Any choice is as correct
			// as any other; what matters is that it sweeps rather than collapsing.
			if (d < -0.9995f)
			{
				float axis[3] = { a[1] * 1.0f - a[2] * 0.0f,
				                  a[2] * 0.0f - a[0] * 1.0f,
				                  0.0f };                       // a x (0,0,1)
				if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-6f)
				{
					axis[0] = 0.0f;                              // a x (1,0,0)
					axis[1] = a[2];
					axis[2] = -a[1];
				}
				norm(axis);

				const float ang = 3.14159265358979f * t;
				const float s = sinf(ang), c = cosf(ang);
				// Rodrigues, with the dot term dropped: axis is perpendicular
				// to `a`, so (axis . a) is zero by construction.
				out[0] = a[0] * c + (axis[1] * a[2] - axis[2] * a[1]) * s;
				out[1] = a[1] * c + (axis[2] * a[0] - axis[0] * a[2]) * s;
				out[2] = a[2] * c + (axis[0] * a[1] - axis[1] * a[0]) * s;
				norm(out);
				return;
			}

			const float th = acosf(d), s = sinf(th);
			const float wa = sinf((1.0f - t) * th) / s;
			const float wb = sinf(t * th) / s;
			for (int i = 0; i < 3; ++i) out[i] = a[i] * wa + b[i] * wb;
			norm(out);
		}

		// One light, blended. `a` is the keyframe on the left and supplies
		// everything that cannot be interpolated - type, flags and enabled. Half
		// a point light is not a thing, and a shadow flag cannot be 40% set.
		lightmodel::Entry blend(const lightmodel::Entry& ea,
		                        const lightmodel::Entry& eb, float t)
		{
			lightmodel::Entry out = ea;
			const lightbuild::LightParams& a = ea.p;
			const lightbuild::LightParams& b = eb.p;
			lightbuild::LightParams& o = out.p;

			for (int i = 0; i < 3; ++i)
			{
				o.pos[i]    = lerpf(a.pos[i],    b.pos[i],    t);
				o.colour[i] = lerpf(a.colour[i], b.colour[i], t);
			}
			o.intensity  = lerpf(a.intensity,  b.intensity,  t);
			o.range      = lerpf(a.range,      b.range,      t);
			o.falloff    = lerpf(a.falloff,    b.falloff,    t);
			o.innerAngle = lerpf(a.innerAngle, b.innerAngle, t);
			o.outerAngle = lerpf(a.outerAngle, b.outerAngle, t);

			o.volIntensity = lerpf(a.volIntensity, b.volIntensity, t);
			o.volSize      = lerpf(a.volSize,      b.volSize,      t);
			o.volExponent  = lerpf(a.volExponent,  b.volExponent,  t);

			slerpDir(a.dir, b.dir, t, o.dir);
			return out;
		}

		// ---------------------------------------------------------------------
		//  Which keyframes still have a marker
		// ---------------------------------------------------------------------
		//  Deleting a marker does not delete its keyframe - nothing tells us it
		//  happened. rsettings has the same orphans and they are harmless there
		//  because it only ever LOOKS UP an existing marker's time; an orphan is
		//  simply never asked for. A track is different: it is evaluated across
		//  all of its keys, so an orphan keeps animating a light through a marker
		//  that no longer exists.
		//
		//  So the track is filtered against the live markers rather than pruned.
		//  Filtering costs a scan; pruning would cost data - dragging a marker
		//  changes its time, and a prune would delete the keyframe rather than
		//  leave it dormant until the marker comes back.
		std::vector<long> g_liveKeys;

		void refreshLiveKeys()
		{
			g_liveKeys.clear();

			void* storage = game::markerStorage();
			if (!storage) return;

			const int n = rstorage::markerCount(storage);
			for (int i = 0; i < n; ++i)
				if (void* m = rstorage::tryGetMarker(storage, i))
					g_liveKeys.push_back((long)llroundf(rmarker::timeMs(m)));
		}

		// An EMPTY live set means "could not read the markers", not "there are
		// none". Filtering everything out on that would blink the lights off, so
		// it falls through to using the track as-is.
		inline bool liveKey(long k)
		{
			if (g_liveKeys.empty()) return true;
			for (long v : g_liveKeys) if (v == k) return true;
			return false;
		}

		// The whole set at a point in time.
		//
		// Lights are matched by INDEX across keyframes, which is what makes
		// "the same light" mean anything here. A light present in one keyframe
		// and not the other simply holds - it is not faded in, because there is
		// no second value to fade from.
		Set evalAt(const Track& tr, float now)
		{
			if (tr.empty()) return Set{};

			const long key = (long)llroundf(now);

			// One ordered pass over the live keys only. std::map is sorted, so
			// the last one at or before `key` is the left bracket and the first
			// one after it is the right.
			const Set* lo = nullptr; long loKey = 0;
			const Set* hi = nullptr; long hiKey = 0;
			for (const auto& kv : tr)
			{
				if (!liveKey(kv.first)) continue;
				if (kv.first <= key) { lo = &kv.second; loKey = kv.first; }
				else if (!hi)        { hi = &kv.second; hiKey = kv.first; break; }
			}

			if (!lo && !hi) return Set{};
			if (!lo) return *hi;   // before the first: hold
			if (!hi) return *lo;   // after the last: hold

			const float span = (float)(hiKey - loKey);
			if (span <= 0.0f) return *lo;

			float t = ((float)key - (float)loKey) / span;
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;

			Set out = *lo;
			const int n = lo->count < hi->count ? lo->count : hi->count;
			for (int i = 0; i < n; ++i) out.lights[i] = blend(lo->lights[i], hi->lights[i], t);
			return out;
		}

		// ---------------------------------------------------------------------
		//  Scope
		// ---------------------------------------------------------------------

		Track* trackFor(const std::string& project, int clip)
		{
			auto p = g_store.find(project);
			if (p == g_store.end()) return nullptr;
			auto c = p->second.find(clip);
			return c == p->second.end() ? nullptr : &c->second;
		}

		// The marker the editor currently has OPEN, as a track key.
		// -1 when there is no answer, which is a reason to leave the scope alone
		// rather than to change it.
		//
		// menu::currentEditMarker, NOT the director's current marker. The
		// director's follows the PLAYHEAD; selecting a marker in the editor moves
		// the open one and does not necessarily move that. Keyed on the
		// director's, stepping to another marker left the scope where it was and
		// the next edit went into the previously-open marker's keyframe.
		long currentKey()
		{
			void* m = menu::currentEditMarker();
			if (!m) return -1;
			return (long)llroundf(rmarker::timeMs(m));
		}

		float replayNow()
		{
			return game::addr_g_ReplayTimeMs ? *(float*)game::addr_g_ReplayTimeMs
			                                 : 0.0f;
		}

		// File what is on screen under the keyframe it belongs to.
		//
		// Does NOT drop a camera grab, though the scope changes below must -
		// releasing goes through commit(), which comes back here through
		// noteEdit(), and a grab() call from inside stash() would recurse.
		// Release first, then stash; never the other way round.
		void stash()
		{
			if (g_project.empty() || g_clip < 0) return;

			Track& tr = g_store[g_project][g_clip];
			const lightmodel::Snapshot& now = lightmodel::Editing();
			tr[g_key] = now;

			// Type and flags are not animatable - blend() takes them from the
			// left keyframe, so a light that is a spot at one marker and a point
			// at the next would simply snap. Rather than leave that as a trap,
			// keep them consistent across the whole track: they describe WHAT the
			// light is, which does not change partway through a clip.
			//
			// `enabled` is deliberately NOT propagated. Switching a light on at a
			// marker is a real thing to want, and it steps at the keyframe.
			for (auto& kf : tr)
			{
				if (kf.first == g_key) continue;
				Set& s = kf.second;
				const int n = s.count < now.count ? s.count : now.count;
				for (int i = 0; i < n; ++i)
				{
					s.lights[i].p.type  = now.lights[i].p.type;
					s.lights[i].p.flags = now.lights[i].p.flags;
				}
			}

			markDirty();
		}

		// Apply a structural change to every keyframe. See the header: light
		// identity is the index, so it has to mean the same thing at all of them.
		template <class Fn>
		void forEachKeyframe(Fn fn)
		{
			if (g_project.empty() || g_clip < 0) return;
			Track* tr = trackFor(g_project, g_clip);
			if (!tr) return;
			for (auto& kf : *tr) fn(kf.second);
			markDirty();
		}

		// Load the set the menu should be editing at `key`.
		//
		// An exact keyframe is used as-is. Anything else is the INTERPOLATED
		// value at that point, so a marker with no keyframe of its own opens
		// showing what the light actually looks like there - and the first edit
		// keys it from that, rather than from whatever the previous marker held.
		void loadInto(const Track* tr, long key)
		{
			if (!tr || tr->empty()) { lightmodel::SetAll(g_defaults); return; }

			auto it = tr->find(key);
			lightmodel::SetAll(it != tr->end() ? it->second : evalAt(*tr, (float)key));
		}

		void writeLight(std::ostream& o, const lightmodel::Entry& e)
		{
			const lightbuild::LightParams& p = e.p;
			const bool spot = p.type == lightbuild::kSpot;

			o << "light " << (e.enabled ? "on" : "off")
			  << " type=" << (spot ? "spot" : "point")
			  << " x=" << p.pos[0] << " y=" << p.pos[1] << " z=" << p.pos[2]
			  << " r=" << p.colour[0] << " g=" << p.colour[1] << " b=" << p.colour[2]
			  << " int=" << p.intensity
			  << " range=" << p.range
			  << " falloff=" << p.falloff;

			// The direction is stored as the vector the model actually holds
			// rather than as the ini's pitch/yaw. The ini pair is a nicer thing
			// to type; this file is written by the mod, so the round trip
			// matters more than the ergonomics.
			if (spot)
			{
				o << " dirx=" << p.dir[0] << " diry=" << p.dir[1]
				  << " dirz=" << p.dir[2]
				  << " inner=" << p.innerAngle << " outer=" << p.outerAngle;
			}

			if (p.flags & lightbuild::kVolumeDrawing)
			{
				o << " volint=" << p.volIntensity
				  << " volsize=" << p.volSize
				  << " volexp=" << p.volExponent;
			}

			char buf[32];
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08X", p.flags);
			o << " flags=" << buf;
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%06X",
			            p.timeFlags & lightbuild::kTimeAllHours);
			o << " time=" << buf << '\n';
		}
	}

	bool scoped() { return g_scoped.load(std::memory_order_acquire); }

	// LIVE keyframes only.
	//
	// The raw track size counts orphans - keyframes whose marker has since been
	// deleted. Those are correctly ignored when the track is evaluated, so
	// reporting them here claimed six keys on a light that moves between two,
	// which is worse than showing nothing: the number is only useful if it
	// describes what actually plays.
	int keyCount()
	{
		if (!g_scoped.load(std::memory_order_relaxed)) return -1;
		const Track* tr = trackFor(g_project, g_clip);
		if (!tr) return 0;

		int n = 0;
		for (const auto& kv : *tr) if (liveKey(kv.first)) ++n;
		return n;
	}

	long editingKey()
	{
		return g_scoped.load(std::memory_order_relaxed) ? g_key : -1;
	}

	void noteEdit()
	{
		if (!g_scoped.load(std::memory_order_relaxed)) return;
		stash();
	}

	void lightAdded(int index)
	{
		if (!g_scoped.load(std::memory_order_relaxed) || index < 0) return;

		// The new light exists at every keyframe from the moment it is created,
		// holding the value it was placed with. The alternative - existing only
		// where it has been keyed - means a light that blinks out the instant you
		// scrub off the marker you made it on.
		const lightmodel::Snapshot& now = lightmodel::Editing();
		if (index >= now.count) return;
		const lightmodel::Entry e = now.lights[index];

		forEachKeyframe([&](Set& s) {
			if (s.count >= lightmodel::kMaxLights) return;
			const int at = index < s.count ? index : s.count;
			for (int j = s.count; j > at; --j) s.lights[j] = s.lights[j - 1];
			s.lights[at] = e;
			++s.count;
		});
	}

	void lightRemoved(int index)
	{
		if (!g_scoped.load(std::memory_order_relaxed) || index < 0) return;

		forEachKeyframe([&](Set& s) {
			if (index >= s.count) return;
			for (int j = index; j + 1 < s.count; ++j) s.lights[j] = s.lights[j + 1];
			--s.count;
		});
	}

	void load()
	{
		g_path = paths::sub("lights") + "scenelights.txt";

		std::ifstream in(g_path);
		if (!in) return;   // nothing saved yet is the normal first run

		std::string header;
		std::getline(in, header);
		if (header.rfind(kHeader, 0) != 0)
		{
			logger::write("info",
				"lights: '%s' has an unrecognised header - ignoring", g_path.c_str());
			return;
		}
		const int ver = atoi(header.c_str() + strlen(kHeader));

		std::string project;
		int clip = 0, n = 0;
		// v1 had no marker lines at all - every light in a clip was one constant
		// set. Landing those on a single keyframe at 0 reproduces exactly that,
		// because a one-keyframe track holds its value everywhere.
		long key = 0;
		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream ls(line);
			std::string what;
			if (!(ls >> what)) continue;

			if (what == "project")
			{
				// The whole rest of the line: project names contain spaces, and
				// >> would take only the first word - which would then file
				// 'Project 20' and 'Project 21' under the same key.
				std::getline(ls, project);
				const size_t s = project.find_first_not_of(" \t");
				project = (s == std::string::npos) ? std::string() : project.substr(s);
				clip = 0;
				key  = 0;
				continue;
			}

			if (what == "clip")
			{
				ls >> clip;
				key = 0;
				// Materialise it here, not on the first light line, so a clip
				// deliberately left with no lights stays "empty" instead of
				// reading back as "never lit" and re-seeding from the ini.
				if (!project.empty()) g_store[project][clip];
				continue;
			}

			if (what == "marker")
			{
				long t = 0;
				if (ls >> t) key = t;
				if (!project.empty()) g_store[project][clip][key];
				continue;
			}

			if (what != "light" || project.empty()) continue;

			Set& set = g_store[project][clip][key];
			if (set.count >= lightmodel::kMaxLights) continue;

			lightmodel::Entry e;
			lightbuild::LightParams& p = e.p;

			std::string field;
			while (ls >> field)
			{
				if (field == "on")  { e.enabled = true;  continue; }
				if (field == "off") { e.enabled = false; continue; }

				const size_t eq = field.find('=');
				if (eq == std::string::npos) continue;
				const std::string k = field.substr(0, eq);
				const char* val = field.c_str() + eq + 1;

				if      (k == "type")    p.type = strcmp(val, "spot") == 0
				                                      ? lightbuild::kSpot
				                                      : lightbuild::kPoint;
				else if (k == "x")       p.pos[0] = F(val);
				else if (k == "y")       p.pos[1] = F(val);
				else if (k == "z")       p.pos[2] = F(val);
				else if (k == "r")       p.colour[0] = F(val);
				else if (k == "g")       p.colour[1] = F(val);
				else if (k == "b")       p.colour[2] = F(val);
				else if (k == "int")     p.intensity = F(val);
				else if (k == "range")   p.range = F(val);
				else if (k == "falloff") p.falloff = F(val);
				else if (k == "dirx")    p.dir[0] = F(val);
				else if (k == "diry")    p.dir[1] = F(val);
				else if (k == "dirz")    p.dir[2] = F(val);
				else if (k == "inner")   p.innerAngle = F(val);
				else if (k == "outer")   p.outerAngle = F(val);
				else if (k == "volint")  p.volIntensity = F(val);
				else if (k == "volsize") p.volSize = F(val);
				else if (k == "volexp")  p.volExponent = F(val);
				// Base 0, so the 0x written by save() is read as hex and a
				// hand-typed decimal still works.
				else if (k == "flags")   p.flags = (uint32_t)strtoul(val, nullptr, 0);
				else if (k == "time")    p.timeFlags =
				                             (uint32_t)strtoul(val, nullptr, 0) &
				                             lightbuild::kTimeAllHours;
			}

			set.lights[set.count++] = e;
			++n;
		}

		logger::write("info",
			"lights: %d keyframed light(s) across %d project(s) in %s (v%d)",
			n, (int)g_store.size(), g_path.c_str(), ver);

		// A v1 file has no marker lines, so everything landed on key 0 above.
		// Rewriting it as v2 is not urgent, but doing it on the next flush stops
		// the file being one hand-edit away from ambiguity.
		if (ver < kVersion && n > 0) markDirty();
	}

	void syncScope()
	{
		const int clip = game::clipIndex();

		// Leaving the editor hands the lights back to the ini set and lets its
		// reload thread start watching the file again.
		//
		// Tested on "is there a clip", NOT on isEditModeActive(). A full-project
		// preview or an export runs in a DIFFERENT replay mode - game.h says so
		// on replayMode() - so the edit-mode test goes false the moment a render
		// starts. That unbound the store mid-export and swapped every light back
		// to the ini set, which is why nothing showed in a rendered clip.
		//
		// Not tested on a null project name either: a null is also what a
		// project mid-load and a closing editor read as, and unbinding on that
		// transient would file the outgoing keyframe under whatever came next.
		if (clip < 0)
		{
			if (g_scoped.load(std::memory_order_relaxed))
			{
				lights::grab(-1);   // before stash(): it persists through commit()
				stash();
				lightmodel::SetAll(g_defaults);
				lightmodel::Publish();
				g_project.clear();
				g_clip = -1;
				g_key  = 0;
				g_scoped.store(false, std::memory_order_release);
			}
			return;
		}

		// Which markers still exist, refreshed here because this is the one entry
		// point that runs every frame the editor is up. animate() and keyCount()
		// both read the result, and animate() early-outs in cases (a light being
		// grabbed, a single-keyframe track) where a stale list would still be
		// visible in the menu's key count.
		refreshLiveKeys();

		const char* name = game::projectName();
		if (!name || !*name) return;

		// -1 means there is no open marker to read. That is normal during an
		// export, which has no marker selected at all - so it must not block a
		// CLIP change, or a render would keep playing the first clip's lights
		// over every clip after it. It only pins the editing key.
		const long key = currentKey();

		const bool wasScoped = g_scoped.load(std::memory_order_relaxed);
		const bool sameClip  = wasScoped && g_clip == clip && g_project == name;
		if (sameClip && (key < 0 || key == g_key)) return;

		if (wasScoped)
		{
			// Release first. The grabbed index points into the set that is about
			// to be replaced, so carrying it across would drag a light of the
			// INCOMING keyframe to wherever the camera is sitting.
			lights::grab(-1);
			stash();   // the outgoing keyframe keeps what is on screen
		}
		else
		{
			// First scope of this editor session. Whatever the ini set is right
			// now is what every unlit clip starts from. Current() rather than
			// Editing(): the editing copy is seeded once and can be stale if
			// the reload thread has picked up a file change since.
			g_defaults = lightmodel::Current();
		}

		g_project = name;
		g_clip    = clip;
		// Keep the last real key when there is no open marker. A render has
		// none, and guessing 0 there would file any later edit against a
		// keyframe the user never chose.
		if (key >= 0) g_key = key;
		g_scoped.store(true, std::memory_order_release);

		const Track* tr = trackFor(g_project, g_clip);
		loadInto(tr, g_key);
		lightmodel::Publish();

		// Only worth a line when the CLIP changed. Stepping between markers is
		// something you do constantly, and logging it would bury everything else.
		if (!sameClip)
		{
			logger::write("info", "lights: clip %d of '%s' - %d keyframe(s)%s",
				g_clip, g_project.c_str(), tr ? (int)tr->size() : 0,
				tr ? "" : " (not lit before - started from the ini set)");
		}
	}

	void animate()
	{
		if (!g_scoped.load(std::memory_order_relaxed)) return;

		// Authoring beats playback. tickGrab is writing the light from the
		// camera every frame; evaluating the track over the top of it would peg
		// the light to its keyframe and the grab would look completely dead.
		if (lights::grabbed() >= 0) return;

		// g_liveKeys is refreshed by syncScope, which ran just before this.
		const Track* tr = trackFor(g_project, g_clip);
		if (!tr || tr->size() < 2) return;   // nothing to animate between

		// Straight to the render side, NOT through the editing copy: the menu is
		// editing one keyframe, and the value on screen at this instant is a
		// blend that belongs to no keyframe at all. Writing it into the edit copy
		// would let a scrub silently overwrite the keyframe being edited.
		lightmodel::PublishSet(evalAt(*tr, replayNow()));
	}

	void tick()
	{
		if (!g_dirty) return;

		// ~0.5s of quiet, same as the marker side-car: long enough to swallow a
		// held adjustment, short enough that a crash costs one edit.
		if (GetTickCount() - g_dirtyAt < 500) return;
		save();
	}

	void save()
	{
		if (!g_dirty) return;

		if (g_path.empty())
		{
			logger::write("info", "lights: !! per-clip lights pending with no file - not saved");
			clearDirty();
			return;
		}

		std::ofstream out(g_path, std::ofstream::out | std::ofstream::trunc);
		if (!out)
		{
			logger::write("info", "lights: !! could not write %s", g_path.c_str());
			return;
		}

		// A float needs 9 significant digits to survive a text round trip, and
		// the stream default is 6. At 6 a light placed at x=-2119.6842 reads
		// back as -2119.68 - under a centimetre, but it drifts a little further
		// every save, and the whole point of this file is that it is stable.
		out << std::setprecision(9);
		out << kHeader << kVersion << '\n';

		for (const auto& pr : g_store)
		{
			if (pr.first.empty()) continue;
			out << "project " << pr.first << '\n';
			for (const auto& cl : pr.second)
			{
				out << "clip " << cl.first << '\n';
				for (const auto& kf : cl.second)
				{
					out << "marker " << kf.first << '\n';
					const Set& s = kf.second;
					for (int i = 0; i < s.count && i < lightmodel::kMaxLights; ++i)
						writeLight(out, s.lights[i]);
				}
			}
		}

		clearDirty();
	}
}
