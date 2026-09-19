// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "game/worldprobe.h"
#include "game/game.h"
#include "game/signatures.h"
#include "utils/log.h"

#include <cstring>

namespace worldprobe
{
	namespace
	{
		// The descriptor is ~0x888 bytes and the results object owns a 32-entry
		// array of 0x40 each. Both are static rather than stack locals: this is
		// called from the camera update, and putting 2 KB plus 2 KB on that stack
		// every frame is a needless risk for something only ever used one at a
		// time on one thread.
		alignas(16) uint8_t s_desc[gsig::SHAPETEST_DESC_BYTES];
		alignas(16) uint8_t s_results[gsig::SHAPETEST_RES_BYTES];
		alignas(16) uint8_t s_entries[gsig::SHAPETEST_ENTRY_BYTES * 32];

		constexpr int kCapacity = 32;

		using FnManager = void* (__fastcall*)();
		using FnSubmit  = char  (__fastcall*)(void*, void*, int);

		template <typename T> void wr(int off, T v)
		{
			memcpy(s_desc + off, &v, sizeof(T));
		}

		// Build the descriptor from scratch.
		//
		// The game's own ctor is not called: Enhanced has it inlined into the
		// only function that uses it, so there is no call to reach. Every value
		// below is what that ctor writes, read off the Legacy copy - see
		// SHAPETEST_INIT_* in signatures.h. The vtable is the one thing that
		// cannot be reconstructed, and it is derived per build.
		void build(const float start[3], const float end[3])
		{
			memset(s_desc, 0, sizeof(s_desc));

			wr<uint64_t>(0x000, (uint64_t)game::addr_ShapeTestVTable);

			// MIRROR THE WORKING SWEEP, and deviate on ONE field only.
			//
			// The first version took the ctor's defaults for the type, the radius
			// and the options, on the reasoning that a line probe is what focus
			// wants. It returned distances that were plausible and wrong: a
			// crosshair sitting on a car reported 351 m, and one aimed at the road
			// reported nothing hit at all - which a downward ray cannot do in
			// 400 m. The camera basis was verified correct at the same time, so
			// the fault was never the ray.
			//
			// Of the four fields that differed from the free camera's own sweep,
			// exactly one was a decision: the mask has to include vehicles and
			// peds, so the sweep's map-only 0x84000004 is no use. The other three
			// were assumptions - "type 2 is a line probe" was inferred from a ctor
			// default and never confirmed, and the options value was not even
			// noticed. So they now copy the configuration that demonstrably
			// works, and only the mask stays ours.
			//
			// The cost is a capsule instead of a ray. At 2 cm it is a ray for any
			// purpose focus cares about - a subject that two centimetres of shell
			// radius would confuse is one no aperture this size could separate.
			wr<uint32_t>(gsig::SHAPETEST_OFF_TYPE,    gsig::SHAPETEST_TYPE_CAPSULE);
			wr<float>   (gsig::SHAPETEST_OFF_RADIUS,  0.02f);
			wr<uint32_t>(gsig::SHAPETEST_OFF_OPTIONS, 2);

			wr<uint32_t>(gsig::SHAPETEST_OFF_ZERO1,   0);
			wr<uint32_t>(gsig::SHAPETEST_OFF_ZERO2,   0);
			wr<uint32_t>(gsig::SHAPETEST_OFF_MASK,    gsig::SHAPETEST_MASK_ALL);
			wr<uint32_t>(gsig::SHAPETEST_OFF_FLAGS1,  gsig::SHAPETEST_INIT_FLAGS1);
			wr<uint8_t> (gsig::SHAPETEST_OFF_FLAGS2,  (uint8_t)gsig::SHAPETEST_INIT_FLAGS2);
			wr<uint32_t>(gsig::SHAPETEST_OFF_CAP,     (uint32_t)kCapacity);
			wr<uint8_t> (gsig::SHAPETEST_OFF_ZERO3,   0);
			wr<uint8_t> (gsig::SHAPETEST_OFF_ZERO4,   0);

			for (int i = 0; i < 3; ++i)
			{
				wr<float>(gsig::SHAPETEST_OFF_START + i * 4, start[i]);
				wr<float>(gsig::SHAPETEST_OFF_END   + i * 4, end[i]);
			}

			// Results, reset every time. The 0xFFFF stamp at +0x38 of each entry
			// is what the engine's own reset writes; an entry left at zero there
			// reads as a hit on whatever object id zero happens to be.
			memset(s_results, 0, sizeof(s_results));
			memset(s_entries, 0, sizeof(s_entries));
			for (int i = 0; i < kCapacity; ++i)
			{
				uint16_t mark = 0xFFFF;
				memcpy(s_entries + i * gsig::SHAPETEST_ENTRY_BYTES +
				       gsig::SHAPETEST_ENTRY_MARK, &mark, sizeof(mark));
			}

			s_results[gsig::SHAPETEST_RES_CAPACITY] = (uint8_t)kCapacity;
			s_results[gsig::SHAPETEST_RES_COUNT]    = 0;

			// Left at 0 on purpose: that byte means "this object allocated its
			// own array and will free it". Ours is static, so claiming otherwise
			// invites the engine to hand our .data section to its allocator.
			s_results[gsig::SHAPETEST_RES_OWNS]     = 0;

			void* const arr = s_entries;
			memcpy(s_results + gsig::SHAPETEST_RES_ARRAY, &arr, sizeof(arr));

			// Both, and they are not the same thing: +0x10 is the results OBJECT
			// and +0x18 is its array, cached alongside. The engine's own helper
			// writes the pair together for the same reason.
			wr<uint64_t>(gsig::SHAPETEST_OFF_RESULTS,  (uint64_t)(void*)s_results);
			wr<uint64_t>(gsig::SHAPETEST_OFF_RESARRAY, (uint64_t)arr);
		}
	}

	bool available()
	{
		return game::addr_ShapeTestManager != 0 &&
		       game::addr_ShapeTestSubmit  != 0 &&
		       game::addr_ShapeTestVTable  != 0;
	}

	bool line(const float start[3], const float end[3], Hit* out)
	{
		Hit local;
		if (!out) out = &local;
		*out = Hit{};
		out->x = end[0]; out->y = end[1]; out->z = end[2];

		if (!available() || !start || !end) return false;

		build(start, end);

		bool anyHit = false;
		__try
		{
			void* mgr = ((FnManager)game::addr_ShapeTestManager)();
			if (!mgr) return false;

			const char ok = ((FnSubmit)game::addr_ShapeTestSubmit)(mgr, s_desc, 0);
			out->submitted = (ok != 0);
			if (!ok) return false;

			const uint8_t count = s_results[gsig::SHAPETEST_RES_COUNT];
			out->count = count;
			if (count > 0)
			{
				memcpy(&out->firstT, s_entries + gsig::SHAPETEST_ENTRY_T, sizeof(float));
			}
			if (count == 0) return false;

			// Nearest hit wins. The array is not sorted - the sweep's own helper
			// takes a running minimum over it for exactly this reason.
			float best = 1.0f;
			const int n = (count < kCapacity) ? count : kCapacity;
			for (int i = 0; i < n; ++i)
			{
				float t = 1.0f;
				memcpy(&t, s_entries + i * gsig::SHAPETEST_ENTRY_BYTES +
				       gsig::SHAPETEST_ENTRY_T, sizeof(t));
				if (t > 0.0f && t < best) { best = t; anyHit = true; }
			}
			if (!anyHit) return false;

			out->hit = true;
			out->t   = best;
			for (int i = 0; i < 3; ++i)
			{
				(&out->x)[i] = start[i] + (end[i] - start[i]) * best;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// One bad offset here is a fault inside the physics query, not a
			// wrong number. Say so once and stand down for good rather than
			// fault again on the next frame.
			static bool said = false;
			if (!said)
			{
				said = true;
				logger::write("info",
					"worldprobe: the shape test faulted - standing down. Autofocus "
					"will report nothing under the focus point from here.");
			}
			game::addr_ShapeTestSubmit = 0;
			return false;
		}
		return anyHit;
	}
}
