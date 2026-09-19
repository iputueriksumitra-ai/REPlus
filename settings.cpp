// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "utils/paths.h"
#include "game/game.h"
#include "replay/settings.h"

#include <map>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>

namespace rsettings
{
	namespace
	{
		// Marker times are floats but always whole milliseconds in practice.
		// Quantising to an integer key avoids float-equality lookups failing on
		// a 1-ulp difference between what we stored and what we read back.
		//
		// Scoped by the clip's IDENTITY rather than its index. Index scoping was
		// the previous shape and it misattributed the moment a project was
		// edited: delete clip 2 of 5 and clips 3, 4 and 5 all shift down one, so
		// every one of their markers inherits the settings of the clip that used
		// to sit there. Five clips, delete one, four break - which is exactly the
		// report this replaces. Identity does not move when its neighbours do.
		//
		// A std::pair orders clip-major then time, which is what lets save()
		// emit one `clip` header per group by walking the map in order.
		using Key = std::pair<uint64_t, int32_t>;
		inline Key keyOf(uint64_t clip, float timeMs)
		{
			return Key(clip, (int32_t)llroundf(timeMs));
		}
		inline uint64_t clipOf(const Key& k) { return k.first; }
		inline long     timeOf(const Key& k) { return (long)k.second; }

		// A clip whose real identity is not known yet, keyed by index instead.
		//
		// The high bit is the marker, and game::clipIdentities() MASKS BIT 63 OFF
		// every identity it produces so the two spaces cannot overlap. That is a
		// contract between the two files, not a probability: an FNV-1a hash sets
		// bit 63 about half the time, so without the mask half of all real clips
		// would read back as unresolved and be remapped through the clip list as
		// though their low bits were an index.
		//
		// Two things depend on telling them apart: the reconciler, which must
		// never reap a clip merely because it has not been identified yet, and the
		// migration, which is looking for exactly these.
		constexpr uint64_t kUnresolvedBit = 0x8000000000000000ull;
		inline uint64_t unresolvedId(int index)
		{
			return kUnresolvedBit | (uint64_t)(uint32_t)index;
		}
		inline bool isUnresolved(uint64_t id) { return (id & kUnresolvedBit) != 0; }

		// Which clip lookups are scoped to. Maintained by syncScope(). Starts
		// unresolved-index-0, which is what a freshly opened project is until the
		// montage can be read.
		uint64_t g_clipId = unresolvedId(0);

		// Whether the clip list has been read successfully at least once for the
		// CURRENT project. Everything that DELETES is gated on it.
		//
		// At namespace scope, not a static inside syncScope(), and that is the
		// whole point: as a function-local it survived a project switch, so the
		// first read of the NEXT project was treated as a confirmation and could
		// reap immediately. bindProject clears it.
		bool g_everResolved = false;

		// The previous read's clip list. Reaping requires this read to AGREE with
		// it, not merely to be the second one.
		//
		// "Second read" is not enough on its own: a montage is populated over
		// several frames, so reads one and two can both be short and both be
		// wrong, and reaping against either deletes the settings of every clip
		// that had not been added yet. Two identical reads a second apart is
		// evidence the list has settled. A project genuinely being edited fails
		// this check once and passes on the next pass, which costs a second and
		// removes the whole class of "a hitch ate my settings".
		std::vector<uint64_t> g_lastLive;

		std::map<Key, MarkerSettings> g_entries;

		// The path twice over. g_wpath is what the file calls actually use -
		// project names are UTF-8 and the -A APIs would reinterpret those bytes
		// in the system codepage, turning a Russian project name into mojibake on
		// disk. g_path is the UTF-8 form, kept only so the log can name the file;
		// the log is UTF-8 already, so it prints correctly there.
		std::string                   g_path;
		std::wstring                  g_wpath;

		bool                          g_dirty = false;

		// Tick count of the most recent change, for the debounce in tick().
		// 0 = nothing pending.
		unsigned long                 g_dirtyAt = 0;

		// The camera hook reads these on the sim thread while the ReShade
		// overlay writes them on the render thread. A std::map is not safe
		// under that, so every entry point takes this lock. Contention is nil:
		// the reader does one lookup per frame.
		std::mutex                    g_mutex;

		// Flat text. Deliberately not JSON: no dependency, trivially diffable,
		// and easy to hand-edit if a project needs rescuing.
		//
		//   RockstarEditorPlus v6
		//   clip 0
		//   marker 2000 path=2 orient=1 shake=1 alpha=0.5 easeIn=1
		//   marker 7500 swayPos=0.02
		//   clip 1
		//   marker 0 orient=2
		//
		// A `clip` line sets the scope for the `marker` lines under it. Fields are
		// NAMED and only written when set, which is both far smaller than the old
		// positional block and immune to the reordering problem that left
		// P_UNUSED_SINE stuck as a permanent hole.
		constexpr const char* kHeader  = "RockstarEditorPlus v";
		constexpr int         kVersion = 7;

		// Written on every save and never read by the mod itself. It exists so
		// that a side-car for a project that no longer exists can be identified
		// as stale - we cannot enumerate the game's projects, so "when was this
		// last opened" is the only evidence available. Reported, never acted on
		// automatically: a project you have not touched in a year is not a
		// project you deleted.
		unsigned long long g_lastSeen = 0;

		// How many clips the project held when this was last written. v6 had no
		// equivalent, which is why migrating one cannot verify its own mapping;
		// recording it means the next format change will not have that problem.
		int g_clipCount = 0;

		// The flag and its timestamp are ONE piece of state and must move
		// together. tick() debounces on the timestamp, so a g_dirty raised
		// without one is a change that never gets flushed at all - which is
		// exactly what happened to an adopted legacy side-car: bindProject set
		// the flag to force a rewrite under the new name, left the timestamp at
		// zero, and the file stayed unwritten until the user happened to edit a
		// marker. Clearing had the mirror problem, leaving a stale timestamp from
		// the outgoing project behind.
		//
		// Both callers already hold g_mutex.
		inline void markDirty()
		{
			g_dirty   = true;
			g_dirtyAt = GetTickCount();
		}

		inline void clearDirty()
		{
			g_dirty   = false;
			g_dirtyAt = 0;
		}

		std::wstring widen(const std::string& s, UINT codepage)
		{
			if (s.empty()) return {};
			const int n = MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(),
			                                  nullptr, 0);
			if (n <= 0) return {};
			std::wstring w((size_t)n, L'\0');
			MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), &w[0], n);
			return w;
		}

		// Make a project name safe to put in a filename WITHOUT flattening it.
		//
		// Only the characters Windows actually rejects are replaced, and every one
		// of those is ASCII - so the UTF-8 bytes of a non-Latin name pass through
		// untouched and 'Проект 25' stays 'Проект 25'. The previous version
		// allowed only [A-Za-z0-9 _-], which turned every Cyrillic name into a row
		// of underscores: unreadable, and worse, it collided - two different names
		// of the same byte length produced the same file, which is the exact bug
		// per-project scoping exists to fix.
		std::string sanitise(const char* name)
		{
			std::string out;
			for (const char* p = name; *p; ++p)
			{
				const unsigned char c = (unsigned char)*p;
				const bool illegal = c < 0x20 || c == '<' || c == '>' || c == ':' ||
				                     c == '"' || c == '/' || c == '\\' || c == '|' ||
				                     c == '?' || c == '*' || c == 0x7F;
				out += illegal ? '_' : (char)c;
			}

			// Windows silently drops trailing dots and spaces, so a name ending in
			// one would resolve to a file we did not think we asked for.
			while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

			// Reserved device names. A project called CON or LPT1 is absurd but
			// costs nothing to survive, and the failure without this is an
			// unopenable file rather than anything that explains itself.
			static const char* const kDevices[] = {
				"CON","PRN","AUX","NUL",
				"COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
				"LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9",
			};
			for (const char* d : kDevices)
				if (_stricmp(out.c_str(), d) == 0) { out.insert(out.begin(), '_'); break; }

			return out.empty() ? std::string("project") : out;
		}

		// What sanitise() used to do. Kept solely to find files written by the
		// builds that shipped it, so a non-Latin project can be reunited with its
		// settings instead of appearing to have lost them.
		std::string sanitiseAscii(const char* name)
		{
			std::string out;
			for (const char* p = name; *p; ++p)
			{
				const char c = *p;
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
				out += ok ? c : '_';
			}
			return out;
		}
	}

	// One entry per Param, in enum order. Short, because the audience for these
	// is somebody opening the file to rescue a project by hand.
	//
	// A name is part of the FILE FORMAT once shipped. Renaming one silently
	// drops that value out of every side-car already on disk - the loader will
	// not recognise the old key and skip it. Add freely, rename never.
	const char* paramName(Param p)
	{
		// NO explicit size. Declaring it [P_COUNT] made the assert below vacuous -
		// the array is P_COUNT long by definition, missing entries are just null,
		// and a Param added without a name reaches strcmp as nullptr on load. Let
		// the initialiser size it so the count is the real one.
		static const char* const kNames[] = {
			"alpha", "easeIn", "easeOut",
			"swayPos", "swayRot", "swayFreq",
			"jitPos", "jitRot", "jitFreq",
			"octaves", "seed",
			"axLat", "axFwd", "axVert", "axPitch", "axRoll", "axYaw",
			"simple", "intensity", "freqMul",
			"speedAmp", "speedFreq", "stopStill", "variation",
			"unusedSine",
			"dofAf", "dofDelta",
		};
		static_assert(sizeof(kNames) / sizeof(*kNames) == P_COUNT,
			"a Param was added without a name - it would serialise as \"?\" and "
			"collide with every other unnamed one");
		return (p >= 0 && p < P_COUNT) ? kNames[p] : "?";
	}

	Param paramFromName(const char* name)
	{
		if (!name) return P_COUNT;
		for (int i = 0; i < P_COUNT; ++i)
			if (strcmp(name, paramName((Param)i)) == 0) return (Param)i;
		return P_COUNT;
	}

	MarkerSettings get(float timeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(keyOf(g_clipId, timeMs));
		return it == g_entries.end() ? MarkerSettings{} : it->second;
	}

	void set(float timeMs, const MarkerSettings& s)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key k = keyOf(g_clipId, timeMs);
		if (s.isDefault())
		{
			if (g_entries.erase(k)) markDirty();
		}
		else
		{
			g_entries[k] = s;
			markDirty();
		}
	}

	void scaleParam(Param p, float ratio)
	{
		if (p < 0 || p >= P_COUNT || !(ratio > 0.0f) || ratio == 1.0f) return;

		std::lock_guard<std::mutex> lock(g_mutex);
		bool any = false;
		for (auto& kv : g_entries)
		{
			// has() is >= 0, and scaling an unset -1 would invent an override.
			if (!kv.second.has(p)) continue;
			kv.second.v[p] *= ratio;
			any = true;
		}
		if (any) markDirty();
	}

	void rekey(float oldTimeMs, float newTimeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key from = keyOf(g_clipId, oldTimeMs), to = keyOf(g_clipId, newTimeMs);
		if (from == to) return;

		auto it = g_entries.find(from);
		if (it == g_entries.end()) return;

		MarkerSettings s = it->second;
		g_entries.erase(it);
		g_entries[to] = s;
		markDirty();
	}

	int count()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return (int)g_entries.size();
	}

	void bindProject(const char* projectName)
	{
		save(); // don't lose edits made to the outgoing project

		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries.clear();
		clearDirty();

		// A newly-opened project starts on its first clip. syncScope() corrects
		// this on the next frame if it did not; without the reset, a switch that
		// happens while clipIndex() is briefly -1 would leave lookups scoped to
		// a clip of the OUTGOING project.
		//
		// Unresolved rather than a real identity: the montage is usually still
		// loading at this point, so nothing can be identified yet. syncScope()
		// promotes it as soon as the clip list can be read.
		g_clipId        = unresolvedId(0);
		g_lastSeen      = 0;
		g_clipCount     = 0;
		g_everResolved  = false;
		g_lastLive.clear();

		if (!projectName || !*projectName) { g_path.clear(); g_wpath.clear(); return; }

		const std::string safe = sanitise(projectName);

		char dir[MAX_PATH]{};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)&bindProject, &self);
		GetModuleFileNameA(self, dir, MAX_PATH);
		if (char* slash = strrchr(dir, '\\')) *(slash + 1) = '\0';

		// Per-project marker overrides now live in the mod's markers\ folder.
		// `dir` is still resolved above because the legacy adoption below has to
		// look where older builds wrote.
		//
		// Two encodings meet here, which is why the halves are widened apart: the
		// folder came out of a -A call so it is in the system codepage, while the
		// name came out of the game so it is UTF-8. Widening the concatenation
		// under either one would corrupt the other half.
		const std::string folder = paths::sub("markers");
		g_path  = folder + safe + ".txt";                       // for the log only
		g_wpath = widen(folder, CP_ACP) + widen(safe + ".txt", CP_UTF8);

		// Adopt a game-root file from the previous layout. Moved, not copied, so
		// there is only ever one file holding a project's overrides.
		if (GetFileAttributesW(g_wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			const std::wstring prev = widen(dir, CP_ACP) +
				L"RockstarEditorPlus_" + widen(safe + ".txt", CP_UTF8);
			if (MoveFileW(prev.c_str(), g_wpath.c_str()))
				logger::write("info", "settings: moved a game-root file into markers");

			// Builds between the per-project split and the Unicode filenames
			// mangled every non-ASCII byte to '_', so a project whose name is not
			// Latin has its settings sitting under a row of underscores. Claim it
			// - it is unambiguously this project's, and it was written days ago at
			// most.
			const std::string ascii = sanitiseAscii(projectName);
			if (ascii != safe)
			{
				const std::wstring mangled = widen(folder + ascii + ".txt", CP_ACP);
				if (MoveFileW(mangled.c_str(), g_wpath.c_str()))
					logger::write("info", "settings: adopted '%s.txt' as '%s.txt'",
						ascii.c_str(), safe.c_str());
			}
		}

		std::ifstream in(g_wpath);
		if (!in)
		{
			// The tool used to be called SmoothBlendFix. Adopt an existing
			// side-car from that name rather than silently starting empty and
			// losing per-marker work; the next save writes the new filename.
			const std::wstring legacy = widen(dir, CP_ACP) +
				L"SmoothBlendFix_" + widen(safe + ".txt", CP_UTF8);
			in.open(legacy);
			if (in)
			{
				logger::write("info", "settings: adopting a legacy SmoothBlendFix file");
				markDirty(); // force a rewrite under the new name
			}
		}
		if (!in)
		{
			logger::write("info", "settings: new project '%s'", safe.c_str());

			// Builds before per-project binding put everything in one file called
			// default.txt, whatever project it came from. Point at it rather than
			// adopting it: its entries belong to an unknown mix of projects, so
			// pulling them in here would just move the collision instead of
			// ending it. Renaming it by hand is a decision only the user can make.
			const std::string shared = paths::sub("markers") + "default.txt";
			if (safe != "default" &&
			    GetFileAttributesA(shared.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				logger::write("info",
					"settings: markers\\default.txt is from before per-project "
					"settings and is no longer read - rename it to '%s.txt' to "
					"claim it for this project",
					safe.c_str());
			}
			return;
		}

		std::string header;
		std::getline(in, header);
		if (header.rfind(kHeader, 0) != 0)
		{
			logger::write("info", "settings: '%s' has an unrecognised header - ignoring",
				g_path.c_str());
			return;
		}

		// The v4 layout replaced a fixed field list with a counted parameter
		// block, so an older file's columns mean different things. Parsing one
		// anyway would silently produce wrong values rather than obviously
		// wrong ones, so refuse it and keep the file for manual rescue.
		const int ver = atoi(header.c_str() + strlen(kHeader));
		if (ver < 4)
		{
			logger::write("info",
				"settings: '%s' is v%d, this build writes v%d - not loading "
				"(delete it, or re-set the markers you care about)",
				g_path.c_str(), ver, kVersion);
			return;
		}

		std::string line;
		if (ver < 6)
		{
			// v4/v5: <time> <path> <orient> <count> <values...> [shake]
			//
			// These predate both per-project files and clip scoping, so every
			// entry lands in clip 0. That is a guess, but it is the right one
			// for the common case - a file written before this existed came
			// from a project whose markers were all being edited in one clip.
			while (std::getline(in, line))
			{
				std::istringstream ls(line);
				long long t; int p, o;
				if (!(ls >> t >> p >> o)) continue;

				MarkerSettings s;
				s.path   = (PathMode)(p   < 0 || p > 2 ? 0 : p);
				s.orient = (OrientMode)(o < 0 || o > 2 ? 0 : o);

				int n = 0;
				if (ls >> n)
				{
					const int take = n > P_COUNT ? P_COUNT : n;
					for (int i = 0; i < take; ++i)
					{
						float f;
						if (!(ls >> f)) break;
						s.v[i] = f;
					}
					// Skip parameters a newer build wrote that we do not know
					// about, so the v5 flag after them still lines up.
					for (int i = take; i < n; ++i) { float junk; if (!(ls >> junk)) break; }
				}

				// v5 trailing flag. Absent in a v4 file, which leaves it false.
				int sh = 0;
				if (ls >> sh) s.ourShake = sh != 0;

				g_entries[keyOf(unresolvedId(0), (float)t)] = s;
			}

			// Rewrite in the current format on the next flush, so the file stops
			// being one bad edit away from the positional-column trap.
			markDirty();
		}
		else
		{
			// v6+: `clip <scope>` scopes the `marker` lines that follow it.
			//
			// v6 wrote a decimal clip INDEX there; v7 writes a 16-digit hex clip
			// IDENTITY. They are told apart by the version, not by the shape of
			// the token - a v6 index of 10 and a v7 identity are both "10" to a
			// hex reader, and guessing would silently mis-scope an entire file.
			//
			// A v6 index becomes an UNRESOLVED id, which is the migration's
			// input: syncScope() converts them to real identities once the clip
			// list can be read, and never before.
			//
			// An unrecognised key is skipped rather than failing the line, and
			// an unrecognised LINE is skipped rather than failing the file, so a
			// side-car written by a future build still loads what it can here.
			uint64_t clip = unresolvedId(0);
			while (std::getline(in, line))
			{
				std::istringstream ls(line);
				std::string what;
				if (!(ls >> what)) continue;

				if (what == "clip")
				{
					std::string scope;
					if (!(ls >> scope)) continue;
					if (ver >= 7)
						clip = strtoull(scope.c_str(), nullptr, 16);
					else
						clip = unresolvedId(atoi(scope.c_str()));
					continue;
				}
				if (what == "seen")  { ls >> g_lastSeen;  continue; }
				if (what == "clips") { ls >> g_clipCount; continue; }
				if (what != "marker") continue;

				long long t;
				if (!(ls >> t)) continue;

				MarkerSettings s;
				std::string field;
				while (ls >> field)
				{
					const size_t eq = field.find('=');
					if (eq == std::string::npos) continue;

					const std::string key = field.substr(0, eq);
					const char* val = field.c_str() + eq + 1;

					if (key == "path")
					{
						const int p = atoi(val);
						s.path = (PathMode)(p < 0 || p > 2 ? 0 : p);
					}
					else if (key == "orient")
					{
						const int o = atoi(val);
						s.orient = (OrientMode)(o < 0 || o > 2 ? 0 : o);
					}
					else if (key == "shake")
					{
						s.ourShake = atoi(val) != 0;
					}
					else
					{
						const Param p = paramFromName(key.c_str());
						if (p != P_COUNT) s.v[p] = (float)atof(val);
					}
				}

				g_entries[keyOf(clip, (float)t)] = s;
			}
		}

		logger::write("info", "settings: loaded %d marker override(s) from %s (v%d)",
			(int)g_entries.size(), g_path.c_str(), ver);
	}

	// Follow the editor: re-bind when the open project changes, re-scope when the
	// edited clip changes.
	//
	// Only the PROJECT costs anything - one file per project, all its clips
	// inside - so stepping between clips is a single integer store. That is why
	// this is safe to call every frame.
	void syncScope()
	{
		// A null name is not "no project". It is also what a project mid-load
		// reads as, and what everything reads as while the editor is closing.
		// Unbinding on it would flush and clear the store on a transient, so a
		// null is simply ignored and the last real binding stands until another
		// real one replaces it.
		static std::string bound;

		const char* name = game::projectName();

		// Nothing to write into is worse than writing into the wrong file: an
		// unbound store accepts every edit and silently discards it at flush
		// time, with no file to show for it. So if the project name cannot be
		// read at all, fall back to the shared file this used to always use.
		// Settings still leak between projects there - but they SURVIVE, and the
		// log says why.
		if (!name && bound.empty() && game::isEditModeActive())
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				logger::write("info",
					"settings: could not read the project name - falling back to "
					"the shared markers\\default.txt. Settings will be kept, but "
					"they are shared across projects again.");
			}
			name = "default";
		}

		if (name && bound != name)
		{
			logger::write("info", "settings: project is now '%s'", name);
			bound = name;
			bindProject(name);
		}

		// -1 is "not in the editor". Same argument: keep the last real scope.
		const int clip = game::clipIndex();
		if (clip < 0) return;

		// Re-read the clip list when the edited clip changes, and once a second
		// otherwise.
		//
		// The periodic read is what notices an EDIT. Deleting a clip does not
		// have to change the index - delete clip 3 while sitting on clip 3 and
		// the index is still 3, pointing at what used to be clip 4 - so keying
		// off the index alone would keep writing into the wrong scope until the
		// user happened to move. A montage walk is a handful of pointer reads and
		// this is the only thing that costs anything here, so once a second is
		// both cheap and soon enough to matter.
		static int sinceRead = 0;
		static int lastClip  = -1;

		// A bind resets the guard, and that is also the signal to re-read at once
		// rather than waiting out the timer on a project we know nothing about.
		if (!g_everResolved) { sinceRead = 60; lastClip = -1; }

		const bool due = (clip != lastClip) || (++sinceRead >= 60);
		lastClip = clip;
		if (!due)
		{
			// Scope still has to follow the index while unresolved, or a project
			// whose montage never becomes readable stops distinguishing clips.
			std::lock_guard<std::mutex> lock(g_mutex);
			if (isUnresolved(g_clipId)) g_clipId = unresolvedId(clip);
			return;
		}
		sinceRead = 0;

		std::vector<uint64_t> live;
		if (!game::clipIdentities(live))
		{
			// COULD NOT READ. Not "there are no clips" - a load, a transition, or
			// the editor closing all land here, and every one of them is
			// temporary. Keep the last scope and change nothing else; reaping on
			// this would empty a project's settings for a streaming hitch.
			std::lock_guard<std::mutex> lock(g_mutex);
			if (isUnresolved(g_clipId)) g_clipId = unresolvedId(clip);
			return;
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		// ---- migrate a side-car written when scoping was by index -------------
		//
		// Index-scoped entries arrive as unresolved ids and are converted here,
		// once, against the live clip list. The test for whether that conversion
		// is safe is stated at the branch below.
		bool anyUnresolved = false;
		int  maxIndex      = -1;
		for (const auto& kv : g_entries)
			if (isUnresolved(clipOf(kv.first)))
			{
				anyUnresolved = true;
				const int i = (int)(uint32_t)clipOf(kv.first);
				if (i > maxIndex) maxIndex = i;
			}

		if (anyUnresolved)
		{
			// IN RANGE, not "exactly as many as the project has".
			//
			// The first version of this demanded maxIndex + 1 == live.size(), which
			// looks like a validity check and is not: maxIndex is the highest clip
			// that HAS settings, not how many clips the project had. A perfectly
			// good file with one tweaked marker on clip 0 reports maxIndex 0, so a
			// five-clip project orphaned it every time.
			//
			// v6 does not record the clip count, so an intact mapping genuinely
			// cannot be distinguished from one that predates a deletion. Being
			// permissive keeps whatever association the file already had - which is
			// what the user has been looking at - rather than discarding it on a
			// suspicion. Out of range is different: that file cannot be applied at
			// all, so it is set aside. v7 writes the count so this stops being a
			// judgement call.
			if (maxIndex >= 0 && (size_t)maxIndex < live.size())
			{
				std::map<Key, MarkerSettings> moved;
				for (auto& kv : g_entries)
				{
					const uint64_t c = clipOf(kv.first);
					const uint64_t id = isUnresolved(c)
						? live[(size_t)(uint32_t)c] : c;
					moved[Key(id, kv.first.second)] = kv.second;
				}
				g_entries.swap(moved);
				markDirty();
				logger::write("info",
					"settings: migrated %d marker override(s) from clip-index scoping "
					"to clip identity. If this project had a clip DELETED under an "
					"older build, check the markers still carry the values you expect "
					"- index scoping could not survive that, which is why it changed.",
					(int)g_entries.size());
			}
			else
			{
				// Set aside, never deleted. The user may want to recover values by
				// hand, and this is the only copy of them.
				const std::wstring aside = g_wpath + L".orphan";
				CopyFileW(g_wpath.c_str(), aside.c_str(), FALSE);
				g_entries.clear();
				markDirty();
				logger::write("info",
					"settings: !! this project's side-car refers to clip %d and the "
					"project only holds %d, so its per-marker settings can no "
					"longer be matched to clips. It has been copied to '%s.orphan' and "
					"this project starts clean - the values are still in that file if "
					"you want them back.",
					maxIndex + 1, (int)live.size(), g_path.c_str());
			}
		}

		// ---- drop entries for clips that are no longer in the project ---------
		//
		// Requires a previous read that AGREES with this one. See g_lastLive for
		// why "not the first read" was not a strong enough guard.
		const bool settled = g_everResolved && g_lastLive == live;
		g_lastLive = live;

		if (settled && !g_entries.empty())
		{
			size_t before = g_entries.size();
			for (auto it = g_entries.begin(); it != g_entries.end(); )
			{
				const uint64_t c = clipOf(it->first);
				const bool alive = isUnresolved(c) ||
					std::find(live.begin(), live.end(), c) != live.end();
				it = alive ? std::next(it) : g_entries.erase(it);
			}
			if (g_entries.size() != before)
			{
				markDirty();
				logger::write("info",
					"settings: dropped %d marker override(s) belonging to clip(s) no "
					"longer in this project",
					(int)(before - g_entries.size()));
			}
		}
		g_everResolved = true;

		g_clipCount = (int)live.size();
		if ((size_t)clip < live.size()) g_clipId = live[(size_t)clip];
	}

	// Deferred flush. Call every frame; it writes at most once per quiet period.
	//
	// save() rewrites the WHOLE side-car - open with trunc, walk every entry,
	// close - and the menu used to call it on each left/right step. Held input
	// repeats around 30 times a second, so adjusting one value meant thirty full
	// file rewrites a second, which is what made changing a marker feel heavy in
	// a project with a few markers in it.
	//
	// Nothing needs the file to be current mid-keypress; it only has to be right
	// by the time the game closes or the project changes. So the menu now just
	// dirties the store and this coalesces the burst into one write once the
	// user stops moving. The explicit save() calls that remain are the ones with
	// a real deadline - project switch, shutdown.
	void tick()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_dirty) return;

			// ~0.5s of quiet. Long enough to swallow a held adjustment, short
			// enough that a crash costs one edit rather than a session.
			if (GetTickCount() - g_dirtyAt < 500) return;
		}
		save();
	}

	void save()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_dirty) return;

		// Unbound with pending edits. syncScope()'s fallback exists so this
		// cannot normally happen; say so rather than dropping the write on the
		// floor, which is how "I set a shake and no file appeared" happens.
		if (g_path.empty())
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				logger::write("info",
					"settings: !! %d marker override(s) pending with no project "
					"bound - not saved", (int)g_entries.size());
			}
			return;
		}

		// Write a temporary alongside and swap it in, rather than truncating the
		// real file and writing into the hole.
		//
		// Truncating first means ANY failure past that point loses the whole
		// side-car - every marker in the project, not just the edit being saved.
		// That is not hypothetical: a parameter added without an entry in
		// paramName() streamed a null const char*, which sets failbit and turns
		// every write after it into a no-op, leaving the file empty. The
		// static_assert there stops that particular cause; this stops the class
		// of it, a disk filling up mid-write included.
		const std::wstring tmp = g_wpath + L".tmp";
		{
			std::ofstream out(tmp, std::ofstream::out | std::ofstream::trunc);
			if (!out)
			{
				logger::write("info", "settings: !! could not write %s", g_path.c_str());
				return;
			}

			out << kHeader << kVersion << '\n';
			out << "seen " << (unsigned long long)time(nullptr) << '\n';
			if (g_clipCount > 0) out << "clips " << g_clipCount << '\n';

			// g_entries is keyed clip-major, so iterating it in order already groups
			// by clip - the header only has to be emitted when the scope changes.
			//
			// Hex, zero-padded to sixteen, so the column is readable by eye when
			// somebody opens the file to rescue a project by hand - which is the
			// stated audience for this format.
			uint64_t clip = 0;
			bool     first = true;
			for (const auto& kv : g_entries)
			{
				const MarkerSettings& s = kv.second;
				if (first || clipOf(kv.first) != clip)
				{
					first = false;
					clip  = clipOf(kv.first);
					char id[24];
					snprintf(id, sizeof(id), "%016llx", (unsigned long long)clip);
					out << "clip " << id << '\n';
				}

				out << "marker " << timeOf(kv.first);
				if (s.path   != PathMode::Inherit)   out << " path="   << (int)s.path;
				if (s.orient != OrientMode::Inherit) out << " orient=" << (int)s.orient;
				if (s.ourShake)                      out << " shake=1";
				// Only what is actually set. An inherit is the absence of a field
				// rather than a -1, which keeps a lightly-edited marker to one short
				// line instead of a row of two dozen sentinels.
				for (int i = 0; i < P_COUNT; ++i)
					if (s.has((Param)i)) out << ' ' << paramName((Param)i) << '=' << s.v[i];
				out << '\n';
			}

			out.flush();
			if (!out)
			{
				// Leave the previous file standing and stay dirty, so the next tick
				// tries again instead of the edits quietly evaporating.
				logger::write("info",
					"settings: !! write failed for %s - keeping the previous file",
					g_path.c_str());
				return;
			}
		}

		if (!MoveFileExW(tmp.c_str(), g_wpath.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			logger::write("info",
				"settings: !! could not replace %s (error %lu) - keeping the previous file",
				g_path.c_str(), GetLastError());
			return;
		}

		clearDirty();
	}
}
