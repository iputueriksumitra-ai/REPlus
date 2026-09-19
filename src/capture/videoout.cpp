// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/videoout.h"
#include "capture/exporthook.h"

#include "utils/paths.h"

#include <string>
#include <fstream>
#include <mutex>

namespace videoout
{
	namespace
	{
		HANDLE s_proc    = nullptr;   // ffmpeg
		HANDLE s_stdin   = nullptr;   // our end of its stdin pipe
		bool   s_started = false;
		int    s_pushed  = 0;         // frames handed over
		char   s_outPath[MAX_PATH]{};

		// The renderer still uses render_NNNN as a TEMPORARY working folder.
		// On a successful video render the finished file is published one level
		// up, using the Rockstar Editor project name, then the temporary folder
		// is removed when it is empty. Failed/cancelled renders stay in their
		// numbered folder so diagnostics and any partial output are not lost.
		std::string s_renderFolder;
		std::string s_projectStem;
		std::string s_outExt;

		std::string safeFileStem(const char* raw)
		{
			std::string out = (raw && *raw) ? raw : "Wow Render";

			for (char& ch : out)
			{
				const unsigned char c = (unsigned char)ch;
				if (c < 0x20 || ch == '<' || ch == '>' || ch == ':' ||
				    ch == '"' || ch == '/' || ch == '\\' || ch == '|' ||
				    ch == '?' || ch == '*')
					ch = '_';
			}

			// Windows silently drops trailing dots/spaces from filenames.
			while (!out.empty() && (out.back() == '.' || out.back() == ' '))
				out.pop_back();

			return out.empty() ? std::string("Wow Render") : out;
		}

		std::string parentFolder(const std::string& path)
		{
			const size_t slash = path.find_last_of("\\/");
			return slash == std::string::npos ? std::string() : path.substr(0, slash);
		}

		std::string uniqueProjectOutput(const std::string& base,
		                               const std::string& stem,
		                               const std::string& ext)
		{
			auto make = [&](int n) {
				char suffix[32]{};
				if (n > 0) snprintf(suffix, sizeof(suffix), "_%02d", n);
				return base + "\\" + stem + suffix + "." + ext;
			};

			for (int n = 0; n < 10000; ++n)
			{
				const std::string p = make(n);
				if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES)
					return p;
			}

			// Practically unreachable, but never overwrite a finished export.
			char suffix[48]{};
			snprintf(suffix, sizeof(suffix), "_%lu", GetTickCount());
			return base + "\\" + stem + suffix + "." + ext;
		}

		void publishFinishedVideo()
		{
			if (s_renderFolder.empty() || s_projectStem.empty() ||
			    s_outExt.empty() || !s_outPath[0])
				return;

			const std::string base = parentFolder(s_renderFolder);
			if (base.empty()) return;

			const std::string finalPath =
				uniqueProjectOutput(base, s_projectStem, s_outExt);

			if (!MoveFileExA(s_outPath, finalPath.c_str(),
			                 MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
			{
				logger::write("info",
					"video: finished, but could not publish '%s' as '%s' (error %lu) - "
					"keeping the numbered render folder",
					s_outPath, finalPath.c_str(), GetLastError());
				return;
			}

			logger::write("info", "video: published final export -> %s",
				finalPath.c_str());
			snprintf(s_outPath, sizeof(s_outPath), "%s", finalPath.c_str());

			if (!Config::get().renderKeepFrames)
			{
				// The audio pass is already muxed into the finished video.
				DeleteFileA((s_renderFolder + "\\audio.wav").c_str());
				DeleteFileA((s_renderFolder + "\\assemble.txt").c_str());

				// Frames are deleted as they are fed to ffmpeg when KeepFrames=0,
				// so a successful normal video render should now be empty.
				// If something unexpected remains, leave the folder alone rather
				// than recursively deleting data we did not create here.
				if (!RemoveDirectoryA(s_renderFolder.c_str()))
				{
					const DWORD e = GetLastError();
					if (e != ERROR_DIR_NOT_EMPTY &&
					    e != ERROR_FILE_NOT_FOUND &&
					    e != ERROR_PATH_NOT_FOUND)
						logger::write("info",
							"video: final file published, temporary folder could not "
							"be removed (error %lu): %s", e, s_renderFolder.c_str());
				}
			}
		}

		// ffmpeg's own stdout and stderr, and a thread that does nothing but
		// empty them.
		//
		// IT USED TO INHERIT THE GAME'S HANDLES, and that is what this replaces:
		//
		//     si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		//     si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
		//
		// Whatever those happen to be in GTA5.exe, we do not own them and cannot
		// drain them. When something else has redirected them to a pipe nobody
		// reads - FiveM, a ScriptHookV console, an overlay, another export tool -
		// ffmpeg's first write to stderr fills that pipe's buffer and BLOCKS THERE
		// FOREVER. With -loglevel error it normally writes nothing, so this only
		// bites when ffmpeg has something to report, which is exactly when a render
		// has already gone wrong. Seen as "ffmpeg did not exit" on a finished
		// 873-frame render, whose actual reason went to a handle we never read.
		//
		// A dedicated thread rather than draining from pushFrame: nothing is pushed
		// between the last frame and end(), and that is precisely the window the
		// encoder does its final work in - and the window it hung in.
		HANDLE      s_errRead   = nullptr;
		HANDLE      s_drain     = nullptr;
		std::mutex  s_errMutex;
		std::string s_errTail;        // last kErrTailMax bytes, for the log

		constexpr size_t kErrTailMax = 4096;

		// Bounded, but generous. Measured against the shipped default arguments,
		// the flush after stdin closes is 0.7s at 720p, 1.6s at 1080p and 4.8s at
		// 2160p - seconds, not minutes. So a wait that expires does not mean "slow
		// encoder", it means STUCK; the old 30s was both too short to be sure of
		// that and too long to sit through in silence.
		constexpr DWORD kExitWaitMs = 120000;
		constexpr DWORD kExitTickMs = 10000;

		DWORD WINAPI drainProc(LPVOID)
		{
			char  buf[1024];
			DWORD got = 0;
			while (s_errRead && ReadFile(s_errRead, buf, sizeof(buf), &got, nullptr) && got)
			{
				std::lock_guard<std::mutex> lock(s_errMutex);
				s_errTail.append(buf, got);
				if (s_errTail.size() > kErrTailMax)
					s_errTail.erase(0, s_errTail.size() - kErrTailMax);
			}
			return 0;
		}

		// The last few lines of what ffmpeg said, flattened onto one line so it
		// fits the log's one-line-per-event shape.
		std::string errSummary()
		{
			std::lock_guard<std::mutex> lock(s_errMutex);
			size_t start = s_errTail.size();
			int    lines = 0;
			while (start > 0 && lines < 4)
			{
				const size_t nl = s_errTail.rfind('\n', start - 1);
				if (nl == std::string::npos) { start = 0; break; }
				start = nl;
				++lines;
			}

			std::string out;
			for (size_t i = start; i < s_errTail.size(); ++i)
			{
				const char c = s_errTail[i];
				out += (c == '\r' || c == '\n') ? ' ' : c;
			}
			while (!out.empty() && out.front() == ' ') out.erase(out.begin());
			while (!out.empty() && out.back()  == ' ') out.pop_back();
			return out;
		}

		// Quote a path for a command line. Paths here come from the ini and from
		// the render folder, so they can contain spaces; without this ffmpeg
		// silently treats the tail as another argument.
		std::string quoted(const char* s)
		{
			std::string q = "\"";
			q += s;
			q += "\"";
			return q;
		}

		// --- encoder presets -------------------------------------------------
		//
		// One .ini per codec in the mod's presets folder, each holding the
		// ffmpeg arguments and a matching container. A preset is just those two
		// ini values under a name, so anything expressible in RenderVideoArgs
		// works as a preset - the point is keeping several to hand and
		// switching by name instead of pasting argument strings.
		//
		// Examples are written once if a file is missing, and never overwritten,
		// so edits to them stick.
		void writeDefaultPresets()
		{
			struct P { const char* name; const char* ext; const char* args; const char* note; };

			// COL is the colour tagging every delivery and editing preset wants.
			//
			// The renderer hands over RGB frames, so anything YUV is a
			// conversion. An untagged file leaves the player to guess which
			// primaries it was converted with, and players guess 601 often
			// enough that every hue shifts slightly - an error that survives a
			// whole grade before anyone notices. The two sws flags stop the
			// conversion itself rounding badly on the way.
			#define COL " -sws_flags +accurate_rnd+full_chroma_int" \
			             " -colorspace bt709 -color_primaries bt709" \
			             " -color_trc bt709 -color_range tv"

			static const P kDefaults[] = {
			// --- delivery ------------------------------------------------
			{ "h264",             "mp4", "-c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p" COL,
			  "Near-lossless H.264. Plays anywhere. The safe default." },
			{ "h264_upload",      "mp4", "-c:v libx264 -crf 20 -preset slow -pix_fmt yuv420p" COL,
			  "Smaller H.264 for uploading, where the site re-encodes anyway." },
			{ "h264_80mbps",      "mp4", "-c:v libx264 -b:v 80M -maxrate 96M -bufsize 160M -preset slow -pix_fmt yuv420p" COL,
			  "H.264 master at 80 Mbps target / 96 Mbps peak." },
			{ "h264_100mbps",     "mp4", "-c:v libx264 -b:v 100M -maxrate 120M -bufsize 200M -preset slow -pix_fmt yuv420p" COL,
			  "H.264 master at 100 Mbps target / 120 Mbps peak." },
			{ "h264_120mbps",     "mp4", "-c:v libx264 -b:v 120M -maxrate 144M -bufsize 240M -preset slow -pix_fmt yuv420p" COL,
			  "H.264 master at 120 Mbps target / 144 Mbps peak." },
			{ "h264_150mbps",     "mp4", "-c:v libx264 -b:v 150M -maxrate 180M -bufsize 300M -preset slow -pix_fmt yuv420p" COL,
			  "H.264 master at 150 Mbps target / 180 Mbps peak." },
			{ "h265",             "mp4", "-c:v libx265 -crf 20 -preset slow -pix_fmt yuv420p" COL,
			  "Similar quality at roughly half the size, slower to encode." },
			{ "av1",              "mkv", "-c:v libaom-av1 -crf 25 -b:v 0 -cpu-used 4 -pix_fmt yuv420p" COL,
			  "The best compression here, and the slowest. For uploads." },
			{ "vp9",              "webm","-c:v libvpx-vp9 -crf 28 -b:v 0 -deadline good -cpu-used 2 -pix_fmt yuv420p" COL,
			  "WebM for the browser. Every browser decodes it natively." },

			// --- delivery, on the GPU ------------------------------------
			{ "nvenc_h264",       "mp4", "-c:v h264_nvenc -rc vbr -cq 22 -b:v 0 -preset p7 -tune hq -pix_fmt yuv420p" COL,
			  "GPU H.264. Much faster than x264, larger for the same look." },
			{ "nvenc_hevc",       "mp4", "-c:v hevc_nvenc -rc vbr -cq 22 -b:v 0 -preset p7 -tune hq -pix_fmt yuv420p" COL,
			  "GPU HEVC. The usual pick for long renders - needs an NVIDIA card." },
			{ "nvenc_hevc_10bit", "mp4", "-c:v hevc_nvenc -rc vbr -cq 22 -b:v 0 -preset p7 -tune hq -pix_fmt p010le" COL,
			  "GPU HEVC at 10-bit. Costs nothing extra on the GPU and takes the "
			  "banding out of skies and smoke, which is where 8-bit shows first." },
			{ "nvenc_av1",        "mkv", "-c:v av1_nvenc -rc vbr -cq 25 -b:v 0 -preset p7 -pix_fmt yuv420p" COL,
			  "GPU AV1. Needs an RTX 40-series or newer. libaom quality at a "
			  "fraction of the time." },

			// --- editing intermediates -----------------------------------
			{ "prores_hq",        "mov", "-c:v prores_ks -profile:v hq -pix_fmt yuv422p10le" COL,
			  "ProRes 422 HQ, 10-bit. 4:2:2 - halves colour resolution, which "
			  "shreds saturated text. Fine for ordinary footage." },
			{ "prores_4444",      "mov", "-c:v prores_ks -profile:v 4444 -pix_fmt yuv444p10le" COL,
			  "ProRes 4444. FULL colour resolution - the one to use if anything "
			  "on screen is saturated UI or text." },
			{ "dnxhr_hq",         "mov", "-c:v dnxhd -profile:v dnxhr_hq -pix_fmt yuv422p" COL,
			  "Avid DNxHR HQ, 8-bit 4:2:2. What Resolve and Media Composer prefer." },
			{ "dnxhr_444",        "mov", "-c:v dnxhd -profile:v dnxhr_444 -pix_fmt yuv444p10le" COL,
			  "DNxHR 444, 10-bit full colour. The DNxHR answer to ProRes 4444." },
			{ "cineform",         "mov", "-c:v cfhd -quality film3+ -pix_fmt yuv422p10le" COL,
			  "GoPro CineForm at its highest quality. Visually lossless, smaller "
			  "than ProRes, scrubs just as well." },

			// --- lossless -------------------------------------------------
			{ "utvideo",          "mkv", "-c:v utvideo -pred median -pix_fmt gbrp",
			  "Ut Video, RGB. The practical replacement for Lagarith - bit-exact, "
			  "fast, and there is a free VfW codec so editors read it directly." },
			{ "magicyuv",         "mkv", "-c:v magicyuv -pred median -pix_fmt gbrp",
			  "MagicYUV, RGB. Same idea and size as Ut Video, different VfW codec." },
			{ "ffv1",             "mkv", "-c:v ffv1 -level 3 -pix_fmt yuv444p",
			  "Archival lossless, about a third smaller than Ut Video. Slow to "
			  "scrub - a storage format rather than an editing one." },
			{ "lossless",         "mkv", "-c:v libx264 -qp 0 -preset veryslow -pix_fmt yuv444p",
			  "Mathematically lossless and the smallest of the lossless options "
			  "by far. Slow to encode and slow to scrub." },
			{ "qtrle",            "mov", "-c:v qtrle -pix_fmt rgb24",
			  "Lossless MOV every editor reads with nothing installed. Enormous." },

			// --- share ----------------------------------------------------
			{ "webp",             "webp","-c:v libwebp_anim -lossless 0 -quality 90 -pix_fmt bgra",
			  "Animated WebP - a far better GIF. Full colour, plays inline in "
			  "every current browser." },
			{ "gif",              "gif",
			  "-filter_complex split[a][b];[a]palettegen=stats_mode=diff[p];"
			  "[b][p]paletteuse=dither=bayer:bayer_scale=5[v] -map [v] -c:v gif",
			  "256 colours and no audio. Builds a palette from the clip instead of "
			  "using a generic one, which is worth roughly three times the file "
			  "size AND looks better. Keep it short and drop RenderFps - a small "
			  "MP4 usually beats it on both counts." },
			};

			#undef COL

			const std::string dir = paths::sub("presets");
			for (const P& p : kDefaults)
			{
				const std::string path = dir + p.name + ".ini";
				if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

				std::ofstream f(path);
				if (!f) continue;
				f << "; " << p.note << "\n"
				  << "; Use with RenderVideoPreset=" << p.name
				  << " in Render.ini\n\n"
				  << "[Preset]\n"
				  << "Args=" << p.args << "\n"
				  << "Ext="  << p.ext  << "\n";
			}
		}

		// Fills args/ext from a named preset. False when there is no usable
		// preset by that name, and the caller keeps the ini's own values.
		bool loadPreset(const char* name, std::string& args, std::string& ext)
		{
			if (!name || !*name) return false;

			const std::string path = paths::sub("presets") + name + ".ini";
			if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				logger::write("info", "video: preset '%s' not found - using RenderVideoArgs", name);
				return false;
			}

			char buf[1024]{};
			GetPrivateProfileStringA("Preset", "Args", "", buf, sizeof(buf), path.c_str());
			if (!buf[0])
			{
				logger::write("info", "video: preset '%s' has no Args - using RenderVideoArgs", name);
				return false;
			}
			args = buf;

			char e[64]{};
			GetPrivateProfileStringA("Preset", "Ext", "mp4", e, sizeof(e), path.c_str());
			ext = e;
			return true;
		}

		// Where ffmpeg is looked for, in order.
		//
		// The BUNDLED copy comes first, because that is how this is meant to
		// ship: drop ffmpeg.exe into the mod's folder and video output works
		// with no setup. Searching PATH first would mean whichever unrelated
		// ffmpeg a user happens to have installed silently decides how their
		// renders encode - and those builds vary in which codecs they carry, so
		// a preset that works on one machine fails on another.
		//
		// PATH is still last, so an existing install is used rather than
		// demanding a second copy.
		//
		// Returns empty if none of them work, so begin() reports it once rather
		// than failing per frame.
		std::string findFfmpeg()
		{
			const Config& cfg = Config::get();

			// 1. Explicitly configured. A path that is not there is a mistake
			//    worth naming, not a reason to quietly use a different ffmpeg.
			if (!cfg.ffmpegPath.empty())
			{
				if (GetFileAttributesA(cfg.ffmpegPath.c_str()) != INVALID_FILE_ATTRIBUTES)
					return cfg.ffmpegPath;
				logger::write("info", "video: FfmpegPath '%s' does not exist", cfg.ffmpegPath.c_str());
				return {};
			}

			// 2. Bundled, in the mod's own folder - the intended install.
			const std::string bundled = paths::file("ffmpeg.exe");
			if (GetFileAttributesA(bundled.c_str()) != INVALID_FILE_ATTRIBUTES)
				return bundled;

			// 3. Beside the game, where other tools sometimes leave one.
			const std::string beside = paths::gameDir() + "ffmpeg.exe";
			if (GetFileAttributesA(beside.c_str()) != INVALID_FILE_ATTRIBUTES)
				return beside;

			// 4. Anything on PATH.
			char found[MAX_PATH]{};
			if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr))
				return found;

			return {};
		}
	}

	bool active()
	{
		return s_started && s_stdin != nullptr;
	}

	// The same resolution begin() performs, without starting anything: a named
	// preset's Ext wins over RenderVideoExt.
	//
	// Deliberately reuses loadPreset() rather than re-reading the preset file
	// with its own copy of the section and key names - a display that agreed
	// with the encoder only until someone renamed a key would be worse than no
	// display at all.
	//
	// loadPreset logs when a preset is named but unusable, which would turn a
	// menu row into a log spammer, so a missing preset is resolved quietly here
	// by asking whether the file exists first.
	const char* outputExtension()
	{
		static std::string s_ext;
		const Config& cfg = Config::get();

		s_ext = cfg.renderVideoExt.empty() ? "mp4" : cfg.renderVideoExt;

		if (!cfg.renderVideoPreset.empty())
		{
			const std::string path =
				paths::sub("presets") + cfg.renderVideoPreset + ".ini";
			if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				char e[64]{};
				GetPrivateProfileStringA("Preset", "Ext", s_ext.c_str(),
				                         e, sizeof(e), path.c_str());
				if (e[0]) s_ext = e;
			}
		}
		return s_ext.c_str();
	}

	void presetNames(std::vector<std::string>& out)
	{
		out.clear();
		writeDefaultPresets();

		WIN32_FIND_DATAA fd{};
		const std::string dir = paths::sub("presets");
		HANDLE h = FindFirstFileA((dir + "*.ini").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			std::string n = fd.cFileName;
			const size_t dot = n.rfind('.');
			if (dot != std::string::npos) n = n.substr(0, dot);
			if (!n.empty()) out.push_back(n);
		}
		while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	std::string s_audioOverride;
	void setAudio(const char* path) { s_audioOverride = path ? path : ""; }

	bool begin(const char* folder, float fps)
	{
		s_started = false;
		s_pushed  = 0;
		s_outPath[0] = '\0';

		const Config& cfg = Config::get();
		if (!cfg.wantsVideo()) return false;

		const std::string exe = findFfmpeg();
		if (exe.empty())
		{
			logger::write("info",
				"video: ffmpeg not found - writing frames only.");
			logger::write("info",
				"video: put ffmpeg.exe in %s, or set FfmpegPath in the ini.",
				paths::baseDir().c_str());
			return false;
		}

		writeDefaultPresets();

		// A named preset overrides the inline args. Falling back rather than
		// failing on a bad name would silently produce a differently-encoded
		// video, so loadPreset says so in the log and leaves the ini's values.
		std::string args = cfg.renderVideoArgs;
		std::string ext  = cfg.renderVideoExt;
		if (loadPreset(cfg.renderVideoPreset.c_str(), args, ext))
		{
			logger::write("info", "video: preset '%s'", cfg.renderVideoPreset.c_str());

			// Named right next to the preset that beat them. Two keys that both
			// look like "the encoder settings", where one silently wins, is how
			// a corrected -pix_fmt got edited into an ini and never ran.
			if (!cfg.renderVideoArgs.empty())
				logger::write("info",
					"video: RenderVideoArgs and RenderVideoExt are NOT in use - the preset supplies both. Blank RenderVideoPreset to use them.");
		}

		s_renderFolder = folder ? folder : "";
		s_outExt        = ext;
		{
			// Match original Rockstar Export semantics: the name the user typed
			// immediately before confirming Export is the final filename.
			// Project name is only the fallback when that title could not be
			// observed (for example on a future build whose text UI changed).
			const std::string requested = exporthook::takeRequestedName();
			const char* project = game::projectName();

			if (!requested.empty())
			{
				s_projectStem = safeFileStem(requested.c_str());
				logger::write("info",
					"video: final filename uses Rockstar export title '%s'",
					requested.c_str());
			}
			else
			{
				s_projectStem = safeFileStem(project && *project ? project : "Wow Render");
				logger::write("info",
					"video: final filename falls back to project name '%s'",
					s_projectStem.c_str());
			}
		}

		snprintf(s_outPath, sizeof(s_outPath), "%s\\video.%s", folder, ext.c_str());

		// stdin pipe. Only the READ end is inheritable: ffmpeg needs that one,
		// and leaving our write end inheritable would keep the pipe open in the
		// child, so the encoder would never see EOF and never exit.
		SECURITY_ATTRIBUTES sa{};
		sa.nLength        = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE childRead = nullptr, parentWrite = nullptr;
		if (!CreatePipe(&childRead, &parentWrite, &sa, 1 << 20))
		{
			logger::write("info", "video: could not create the encoder pipe");
			return false;
		}
		SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0);

		// -f image2pipe reads whatever the addon wrote, concatenated. The frame
		// rate is stated BEFORE -i so it applies to the input: image2pipe has no
		// timestamps of its own, and setting it only on the output would leave
		// ffmpeg assuming 25 and duplicating or dropping frames to reach it.
		// Audio comes in as a SECOND input to this same process rather than a
		// remux afterwards: the frames are already streaming through here, so
		// muxing costs nothing extra and there is no intermediate file to clean
		// up or leave behind on a cancelled render.
		//
		// -shortest matters. Our video length is frame count over fps, while the
		// borrowed track is however long the game's export ran, and those agree
		// only approximately - without it the file ends up padded to whichever
		// stream is longer.
		// A wav from our own audio pass wins over AudioFromFile: it was recorded
		// from this project moments ago, where the ini setting is whatever the
		// user last pointed at.
		const std::string src = !s_audioOverride.empty() ? s_audioOverride : cfg.audioFromFile;

		// SOME CONTAINERS HAVE NOWHERE TO PUT AUDIO, and ffmpeg does not shrug
		// that off - it refuses the whole job. "-map 1:a" naming a stream the
		// muxer will not accept ends in "nothing was written into output file",
		// so the render produces NO VIDEO EITHER.
		//
		// Which matters because RenderAudio ships ON. Every one of the share
		// formats - gif, webp, apng - failed outright the moment they were
		// offered as presets, and the failure looked like the preset being
		// broken rather than the muxing.
		//
		// The wav is still written to the render folder by the audio pass, so
		// nothing is lost; it just arrives beside the file instead of inside it.
		bool carriesAudio = true;
		for (const char* mute : { "gif", "webp", "apng" })
			if (_stricmp(ext.c_str(), mute) == 0) carriesAudio = false;

		std::string audio;
		if (!src.empty())
		{
			if (!carriesAudio)
			{
				logger::write("info",
					"video: .%s cannot carry audio - the track is left beside the video as a wav",
					ext.c_str());
			}
			else if (GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				audio = " -i " + quoted(src.c_str()) +
				        " -map 0:v -map 1:a -c:a aac -b:a 320k -shortest";
				logger::write("info", "video: muxing audio from %s", src.c_str());
			}
			else
			{
				logger::write("info", "video: audio source not found, rendering silent - %s",
					src.c_str());
			}
		}

		char cmd[4096];
		const int need = snprintf(cmd, sizeof(cmd),
			"%s -hide_banner -loglevel error -y -f image2pipe -framerate %g -i -%s %s %s",
			quoted(exe.c_str()).c_str(), fps,
			audio.c_str(),
			args.c_str(),
			quoted(s_outPath).c_str());

		// Truncation here is not a cosmetic problem: the OUTPUT PATH is last, so
		// a command line that does not fit loses the destination and ffmpeg
		// either fails or writes somewhere unintended. Preset args alone can run
		// to 1023 characters before any of the three paths are added, so this is
		// reachable rather than theoretical.
		if (need < 0 || need >= (int)sizeof(cmd))
		{
			logger::write("info",
				"video: encoder command line too long (%d chars, limit %d) - "
				"shorten RenderVideoArgs or the output path. Writing frames only.",
				need, (int)sizeof(cmd) - 1);
			return false;
		}

		// A pipe of OUR OWN for ffmpeg's output, so it can always write and we can
		// always say what it wrote. See the note on s_errRead for why inheriting
		// the game's handles was not viable.
		HANDLE errRead = nullptr, errWrite = nullptr;
		if (!CreatePipe(&errRead, &errWrite, &sa, 1 << 16))
		{
			CloseHandle(childRead);
			CloseHandle(parentWrite);
			logger::write("info", "video: could not create the encoder log pipe");
			return false;
		}
		SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOA si{};
		si.cb         = sizeof(si);
		si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdInput  = childRead;
		si.hStdOutput = errWrite;
		si.hStdError  = errWrite;

		PROCESS_INFORMATION pi{};
		const BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

		// Taken BEFORE the handles are closed. CloseHandle sets its own last-error
		// on the way out, so reading it after the cleanup below reported whatever
		// the last close did rather than why the launch failed - and that number is
		// the only clue in a "could not start ffmpeg" report.
		const DWORD launchErr = ok ? 0 : GetLastError();

		// Ours to close either way: on success the child holds its own copy, and
		// on failure nothing should be left holding the pipe open. The log pipe's
		// WRITE end especially - while we hold a copy of that, the read side never
		// reaches end-of-file and the drain thread would never finish.
		CloseHandle(childRead);
		CloseHandle(errWrite);

		if (!ok)
		{
			CloseHandle(parentWrite);
			CloseHandle(errRead);
			logger::write("info", "video: could not start ffmpeg (error %lu)", launchErr);
			return false;
		}

		CloseHandle(pi.hThread);
		s_proc    = pi.hProcess;
		s_stdin   = parentWrite;
		s_errRead = errRead;
		s_started = true;

		{
			std::lock_guard<std::mutex> lock(s_errMutex);
			s_errTail.clear();
		}
		s_drain = CreateThread(nullptr, 0, drainProc, nullptr, 0, nullptr);

		logger::write("info", "video: encoding to %s @ %g fps [%s]",
			s_outPath, fps, args.c_str());
		return true;
	}

	void pushFrame(const char* path)
	{
		if (!active() || !path) return;

		// The addon has signalled the frame is done, but "done writing" and
		// "closed" are not the same instant - a first open can still lose the
		// race. A few short retries cost nothing next to a lost frame, which
		// would desync the whole video from that point on.
		HANDLE f = INVALID_HANDLE_VALUE;
		for (int attempt = 0; attempt < 20; ++attempt)
		{
			f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f != INVALID_HANDLE_VALUE) break;
			Sleep(5);
		}
		if (f == INVALID_HANDLE_VALUE)
		{
			logger::write("info", "video: could not read %s - frame dropped", path);
			return;
		}

		char    buf[64 * 1024];
		DWORD   got = 0;
		bool    bad = false;
		while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
		{
			DWORD off = 0;
			while (off < got)
			{
				DWORD wrote = 0;
				if (!WriteFile(s_stdin, buf + off, got - off, &wrote, nullptr) || wrote == 0)
				{
					bad = true;
					break;
				}
				off += wrote;
			}
			if (bad) break;
		}
		CloseHandle(f);

		if (bad)
		{
			// ffmpeg exited or the pipe broke. Stop feeding it rather than
			// blocking the render thread on every remaining frame.
			logger::write("info", "video: encoder pipe closed early after %d frame(s)", s_pushed);
			CloseHandle(s_stdin);
			s_stdin = nullptr;
			return;
		}

		++s_pushed;
		if (!Config::get().renderKeepFrames) DeleteFileA(path);
	}

	void end(bool complete)
	{
		if (!s_started) return;

		// Closing stdin is what tells ffmpeg the stream ended; without it the
		// encoder waits forever and the container is never finalised.
		if (s_stdin)
		{
			CloseHandle(s_stdin);
			s_stdin = nullptr;
		}

		bool killed = false;
		if (s_proc)
		{
			// Bounded - a hung encoder must not hang the game - but with progress,
			// because a silent stall is indistinguishable from a crash to whoever
			// is sitting in front of it.
			DWORD waited = 0;
			while (WaitForSingleObject(s_proc, kExitTickMs) == WAIT_TIMEOUT)
			{
				waited += kExitTickMs;
				if (waited >= kExitWaitMs) { killed = true; break; }
				logger::write("info",
					"video: still finalising the container after %lus - waiting",
					waited / 1000);
			}

			if (killed)
			{
				// Said in full, because the line this replaces read like a note and
				// the consequence is a lost render.
				//
				// The container advice is measured, not assumed. Killing an encoder
				// 80% of the way through a 400-frame clip and then asking ffmpeg to
				// read the result back:
				//
				//     mp4   9961520 B   moov atom not found        - will not open
				//     mov   9961508 B   moov atom not found        - will not open
				//     mkv   8388608 B   file ended prematurely     - PLAYS
				//
				// mp4 and mov keep their index in a moov atom written at the very
				// end, so the payload on disk is unreachable without it. Matroska
				// writes clusters as it goes and tolerates a missing tail, so the
				// same interruption costs the last second or two instead of
				// everything.
				logger::write("info",
					"video: !! ffmpeg did not exit after %lus - terminating it. THE VIDEO "
					"FILE IS PROBABLY UNUSABLE: mp4 and mov write their index at the very "
					"end, so an encoder killed before it finishes leaves the footage on "
					"disk with no index and players refuse to open it.",
					kExitWaitMs / 1000);
				logger::write("info",
					"video: if this keeps happening, render to MKV - it writes as it goes "
					"and survives an interrupted encode, losing only the last moment "
					"instead of the whole file. The utvideo, ffv1, av1 and lossless "
					"presets already use it.");
				if (!Config::get().renderKeepFrames)
					logger::write("info",
						"video: the frames were deleted as they were encoded, so there is "
						"nothing left to re-assemble from. RenderKeepFrames=1 keeps them "
						"alongside the video.");
				TerminateProcess(s_proc, 1);
				WaitForSingleObject(s_proc, 5000);
			}

			DWORD code = 0;
			GetExitCodeProcess(s_proc, &code);
			CloseHandle(s_proc);
			s_proc = nullptr;

			// The drain thread ends by itself once ffmpeg is gone and the pipe
			// breaks. Joined before the tail is read, so nothing is still arriving.
			if (s_drain)
			{
				WaitForSingleObject(s_drain, 5000);
				CloseHandle(s_drain);
				s_drain = nullptr;
			}
			if (s_errRead) { CloseHandle(s_errRead); s_errRead = nullptr; }

			logger::write("info", "video: %s - %d frame(s) -> %s (ffmpeg exit %lu)",
				killed ? "TERMINATED" : (complete ? "finished" : "ended early"),
				s_pushed, s_outPath, code);

			// What ffmpeg actually said, which is the whole point of owning the
			// pipe: a non-zero exit used to be a bare number, with the reason
			// written to a handle belonging to whatever else is in the process.
			const std::string why = errSummary();
			if (!why.empty())
				logger::write("info", "video: ffmpeg %s: %s",
					(code != 0 || killed) ? "said" : "noted", why.c_str());

			if (complete && !killed && code == 0)
				publishFinishedVideo();
		}

		s_started = false;
		s_pushed  = 0;
		s_renderFolder.clear();
		s_projectStem.clear();
		s_outExt.clear();
	}
}
