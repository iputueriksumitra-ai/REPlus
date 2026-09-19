param(
    [string]$Repo = "."
)

$ErrorActionPreference = "Stop"

function Replace-Exact {
    param(
        [string]$Path,
        [string]$Old,
        [string]$New,
        [string]$Label
    )
    $text = [System.IO.File]::ReadAllText($Path)
    if ($text.Contains($New)) {
        Write-Host "OK (already applied): $Label"
        return
    }
    if (-not $text.Contains($Old)) {
        throw "Patch anchor not found for: $Label`nFile: $Path"
    }
    $text = $text.Replace($Old, $New)
    [System.IO.File]::WriteAllText($Path, $text, [System.Text.UTF8Encoding]::new($false))
    Write-Host "APPLIED: $Label"
}

$video = Join-Path $Repo "src/capture/videoout.cpp"
$render = Join-Path $Repo "src/capture/render.cpp"
$workflow = Join-Path $Repo ".github/workflows/build-wow-cloud-hat.yml"

if (!(Test-Path $video))  { throw "Missing $video" }
if (!(Test-Path $render)) { throw "Missing $render" }

# -------------------------------------------------------------------------
# 1) Direct final output: Captures\Project Name.mp4 instead of
#    Captures\render_NNNN\video.mp4 after a successful render.
# -------------------------------------------------------------------------
$old = @'
		char   s_outPath[MAX_PATH]{};

		// ffmpeg's own stdout and stderr, and a thread that does nothing but
'@

$new = @'
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
'@
Replace-Exact $video $old $new "project-named final output helpers"

# Capture project name + temporary folder after the encoder preset/container
# have been resolved, before ffmpeg's output path is created.
$old = @'
		snprintf(s_outPath, sizeof(s_outPath), "%s\\video.%s", folder, ext.c_str());

		// stdin pipe. Only the READ end is inheritable: ffmpeg needs that one,
'@

$new = @'
		s_renderFolder = folder ? folder : "";
		s_outExt        = ext;
		{
			const char* project = game::projectName();
			s_projectStem = safeFileStem(project && *project ? project : "Wow Render");
		}

		snprintf(s_outPath, sizeof(s_outPath), "%s\\video.%s", folder, ext.c_str());

		// stdin pipe. Only the READ end is inheritable: ffmpeg needs that one,
'@
Replace-Exact $video $old $new "remember project name for final export"

# Move only a SUCCESSFUL, COMPLETE encode. Cancelled or failed renders keep the
# numbered working folder exactly as before.
$old = @'
			const std::string why = errSummary();
			if (!why.empty())
				logger::write("info", "video: ffmpeg %s: %s",
					(code != 0 || killed) ? "said" : "noted", why.c_str());
		}

		s_started = false;
		s_pushed  = 0;
'@

$new = @'
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
'@
Replace-Exact $video $old $new "publish completed video beside render folders"

# -------------------------------------------------------------------------
# 2) Built-in H.264 high-bitrate presets. These are created automatically in
#    RockstarEditorPlus\presets and appear in Encoder Preset.
# -------------------------------------------------------------------------
$old = @'
			{ "h264_upload",      "mp4", "-c:v libx264 -crf 20 -preset slow -pix_fmt yuv420p" COL,
			  "Smaller H.264 for uploading, where the site re-encodes anyway." },
			{ "h265",             "mp4", "-c:v libx265 -crf 20 -preset slow -pix_fmt yuv420p" COL,
'@

$new = @'
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
'@
Replace-Exact $video $old $new "built-in H264 80/100/120/150 Mbps presets"

# -------------------------------------------------------------------------
# 3) FiveM transition guard: the log from the failed test showed BAKE diverted
#    correctly, then REPLAYMODE_DISABLED for only the transition frame. Do not
#    interpret that transient as the editor being permanently closed.
# -------------------------------------------------------------------------
$old = @'
				// The editor closed under us - nothing left to render into.
				if (mode == gsig::REPLAYMODE_DISABLED)
				{
					exporthook::clearPending();
					s_pendWait = 0; s_pendStart = 0;
					logger::write("info",
						"export: not rendering - the editor closed while waiting to start");
					return;
				}
'@

$new = @'
				// FiveM can report DISABLED briefly while Export switches from
				// the menu into full-project playback. The old code treated that
				// transition frame as "the editor closed" and cancelled a valid
				// export before the renderer ever started. Give the transition a
				// short wall-clock grace period; a genuinely closed editor still
				// cancels, only a few seconds later.
				if (mode == gsig::REPLAYMODE_DISABLED)
				{
					if (now - s_pendStart < 5000)
					{
						if (now - s_pendLog >= 1000)
						{
							s_pendLog = now;
							logger::write("info",
								"export: waiting through FiveM editor transition "
								"(replay mode DISABLED)");
						}
						return;
					}

					exporthook::clearPending();
					s_pendWait = 0; s_pendStart = 0;
					logger::write("info",
						"export: not rendering - the editor remained closed for 5 seconds");
					return;
				}
'@
Replace-Exact $render $old $new "FiveM export transition grace period"

# Optional: rename the build artifact so a successful Actions run is obvious.
if (Test-Path $workflow) {
    $wf = [System.IO.File]::ReadAllText($workflow)
    $wf = $wf.Replace("Build WOW Editor V5 Full Renderer", "Build WOW Editor V5.5 Project Output")
    $wf = $wf.Replace("WOW-Editor-V5.3-FiveM-Renderer", "WOW-Editor-V5.5-Project-Named-Output")
    [System.IO.File]::WriteAllText($workflow, $wf, [System.Text.UTF8Encoding]::new($false))
    Write-Host "APPLIED: workflow artifact renamed to V5.5"
}

Write-Host ""
Write-Host "WOW Editor V5.5 patch complete."
Write-Host "Next: commit these changes on branch wow-cloud-hat-extension and push."
Write-Host "GitHub Actions will build the Windows artifact automatically."
