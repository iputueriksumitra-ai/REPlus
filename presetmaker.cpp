// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
//
// =============================================================================
//  RE+ Render Settings - editor for Render.ini and its encoder presets
// =============================================================================
//  Two jobs, one window:
//
//    Render tab   every key in Render.ini, grouped by what it does, with an
//                 explanation attached to each one rather than only in the file
//    Presets tab  builds presets\<name>.ini, the two-line files ffmpeg is
//                 driven by
//
//  Why the preset half exists at all: a container that cannot hold the chosen
//  codec makes ffmpeg refuse, the render produces nothing, and the reason is a
//  line of stderr the user never sees. Here choosing a codec filters the
//  container and pixel-format lists, so the broken combination cannot be
//  expressed. Same for "lossless" that silently subsamples chroma - a lossless
//  codec only offers 4:4:4 or RGB.
//
//  THE SETTINGS ARE GROUPED AND GREYED, and both matter more than they look.
//
//  Grouped, because the previous layout split the table down the middle by
//  COUNT - twelve rows left, eleven right - so which column a setting landed in
//  was decided by how many settings happened to exist above it. Aperture sat
//  under Shutter; Record audio began a column. Reading it told you nothing about
//  what belonged with what.
//
//  Greyed, because a setting that is being ignored looks exactly like one that
//  is working. Highlight boost with motion blur off, JPEG quality with PNG
//  selected, ffmpeg arguments with a preset named over them - each of those is
//  unreachable code in the renderer, and a user who tunes one and sees no change
//  concludes the mod is broken. The in-editor menu already greys its rows for
//  this reason; settingEnabled() below mirrors that function deliberately, and
//  says where it goes beyond it.
//
//  Writing is LINE PRESERVING. Render.ini is heavily commented and those
//  comments are the documentation for anyone editing by hand; rewriting the
//  file (or using WritePrivateProfileString, which drops the trailing comment
//  on any line it touches) would strip them a key at a time.
//
//  Win32, no dependencies, static CRT - same as the .asi, because it ships in
//  the bundle and has to run on a machine with nothing installed.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
//  Settings table - one row per Render.ini key
// ---------------------------------------------------------------------------
enum SType { S_BOOL, S_INT, S_FLOAT, S_TEXT, S_CHOICE, S_PRESET };

// Which box a setting is drawn in. The order here is the order they are drawn;
// kGroups says which column each one goes down.
enum SGroup { G_RENDER, G_BLUR, G_DOF, G_FILES, G_SESSION, G_ENCODE, G_AUDIO, G_COUNT };

struct Group
{
    const char* title;
    int         col;    // 0 = left column, 1 = right column, 2 = full width below
};

// Balanced by row count, not by feel: left carries 10 rows in 2 boxes, right 10
// in 3. Anything added should be checked against that or one column grows past
// the other and the wide boxes underneath start from a ragged edge.
static const Group kGroups[G_COUNT] =
{
    { "Renderer",              0 },
    { "Motion blur",           0 },
    { "Depth of field (IGCS)", 1 },
    { "Image files",           1 },
    { "While rendering",       1 },
    { "Output and encoding",   2 },
    { "Audio",                 2 },
};

struct Setting
{
    const char* key;
    const char* label;
    SType       type;
    SGroup      group;
    const char* choices;   // "Video|Frames"; for S_CHOICE the index is the value
    const char* help;      // tooltip and description strip

    // S_CHOICE only: the ini stores the WORD ("Sliding"), not the index.
    //
    // A table flag rather than a name test in the save path, which is what
    // this was. Adding a second word-valued key meant remembering to extend
    // an `if` three hundred lines away - and forgetting writes "1" into a
    // key the mod compares against "Sliding", so the setting reads back as
    // its default and the tool looks like it did nothing.
    bool        wordValue = false;

    // Accepted range for S_INT / S_FLOAT, clamped when the file is written.
    // lo == hi means unbounded. See clampNumerics() for which of these mirror
    // the mod's own limits and which are the tool's.
    double      lo = 0.0, hi = 0.0;
};

static Setting kSettings[] =
{
// --- Renderer --------------------------------------------------------------
{ "EnableRenderer", "Enable renderer", S_BOOL, G_RENDER, nullptr,
  "The master switch. Export is the only trigger, and this is what diverts it away from the game's own encoder.\r\n"
  "Ships OFF: diverting the bake stops other export tools (EVE, EVER) from running - they do not error, they simply never fire." },

{ "RenderMode", "Output", S_CHOICE, G_RENDER, "Video|Frames",
  "Video   - frames are handed to ffmpeg as they finish and deleted, so a long render costs a couple of files at a time. Audio is muxed in.\r\n"
  "Frames  - numbered PNG/JPEG sequence with an assemble.txt of ready-made ffmpeg commands. Audio lands as audio.wav beside them, and assemble.txt carries the line that attaches it.\r\n"
  "The capture itself is identical either way.", true },

{ "RenderCaptureMode", "Capture mode", S_CHOICE, G_RENDER, "Walking|Sliding",
  "Sliding - the clip PLAYS: it advances to each frame's mark, then exposes Motion blur samples consecutive frames with the world still simulating between them. Particles step, and anything with temporal history (TAA, SSR, ray tracing) stays warm instead of being reset at every sample.\r\n"
  "The default, and about 2x faster than Walking at a 360-degree shutter. Sliding spends one present per sample; Walking spends two - one to redraw at the new time, one to capture it - so it costs 2N+2 presents per frame against sliding's N.\r\n"
  "That edge is shutter-dependent: sliding needs Samples/Shutter presents, so at 0.5 the two are level and below that Walking wins.\r\n"
  "Walking - pause, seek to each sub-sample, grab, average. Exact shutter placement, deterministic frame times, and a more obvious failure mode: a repeated frame rather than a smeared one. Use it if a sliding render looks wrong, or for short shutters.", true },

{ "RenderFps", "Frame rate", S_INT, G_RENDER, nullptr,
  "Output frame rate, independent of the rate the game is running at. Halving it halves the render time.",
  false, 1, 240 },

{ "RenderMarkerSpeed", "Honour marker speed", S_BOOL, G_RENDER, nullptr,
  "Render a marker's SPEED setting - slow motion and fast motion - instead of ignoring it.\r\n"
  "A marker carries a speed of 5 to 200%, and the editor keeps two clocks because of it: the timeline the markers sit on, and real elapsed time. The renderer used to advance the first one uniformly, so a slow-motion section came out at full speed and produced too few frames.\r\n"
  "The audio pass always recorded a real-time playthrough, so it already had the slow motion - which means this also fixes video and audio ending up different lengths on any project that uses speed markers.\r\n"
  "Leave it on. Turn it off only to reproduce a render made by an older build: the frame count and the output duration both change on a project that uses speed markers." },

// --- Motion blur -----------------------------------------------------------
{ "RenderSamples", "Samples per frame", S_INT, G_BLUR, nullptr,
  "Sub-frames averaged into each output frame. 1 = no blur.\r\n"
  "Every sample is a real render at a real instant, so this is true accumulation - and it is the dominant cost. "
  "64 means 64 captures per output frame; drop to 3 while setting a shot up.\r\n"
  "Exact in both capture modes. Walking seeks to each instant; sliding advances the clip to the frame's mark and then takes this many consecutive presents, with the world still simulating between them.\r\n"
  "Greyed out under Depth of field: the aperture sweep does the sampling there and this is pinned to 1.",
  false, 1, 512 },

{ "RenderShutter", "Shutter (1.0 = 360 deg)", S_FLOAT, G_BLUR, nullptr,
  "A FRACTION of the frame interval, not an angle in degrees - the label used to say \"Shutter angle\", which invited entering 180 and produced a 180x exposure that ate the whole clip in a few frames.\r\n"
  "1.0 exposes the whole frame interval (360 degrees). 0.5 is the 180-degree film convention.\r\n"
  "Free in Walking - it only changes how the samples are spaced.\r\n"
  "In Sliding it sets how far the clip is stepped between samples, so a shorter shutter packs the same samples into a narrower slice of time.\r\n"
  "Stays live under Depth of field, where it is the control that matters: it spreads the aperture samples across time, and that is where the motion blur in that mode comes from.",
  false, 0.01, 2.0 },

{ "RenderSettleFrames", "Settle frames", S_INT, G_BLUR, nullptr,
  "Frames to let the game redraw after seeking to a new output frame. Paid once per frame, so it barely matters at high sample counts "
  "but is most of the cost when samples = 1.\r\n"
  "Also what the renderer waits out after a streaming load, in both capture modes, so it is never entirely idle.",
  false, 0, 240 },

{ "RenderSettleSubFrames", "Settle sub-frames", S_INT, G_BLUR, nullptr,
  "Redraw wait between motion-blur sub-samples, in Walking. NEVER 0 - at zero every sample captures the same image and averages back to one sharp frame. The mod raises 0 to 1 on load and so does this tool on save.\r\n"
  "Each extra frame here is multiplied by the sample count, so raising it is expensive.",
  false, 1, 60 },

{ "RenderHighlightBoost", "Highlight boost", S_FLOAT, G_BLUR, nullptr,
  "Lifts bright areas while samples are accumulated in linear light, so speculars streak instead of averaging down to grey. 0 disables it.\r\n"
  "Belongs to the renderer's own accumulation, so it does nothing without motion blur and nothing under Depth of field - that pass hands over one already-accumulated image and has its own highlight controls in the add-on's panel.",
  false, 0.0, 0.99 },

// --- Depth of field --------------------------------------------------------
{ "RenderDepthOfField", "Enable depth of field", S_BOOL, G_DOF, nullptr,
  "Render each frame through a real lens instead of a pinhole. The ReShade add-on accumulates the frame across an actual aperture, so defocus comes from geometry rather than from blurring a finished picture: foreground and background occlude each other correctly, and highlights bloom into the aperture's own shape.\r\n"
  "A MODIFIER, not a mode - it layers onto whichever capture mode is set, and the two combine rather than compete:\r\n"
  "  with Walking  - the clock is frozen for the whole sweep. Depth of field only, no motion blur, and the sharpest possible result.\r\n"
  "  with Sliding  - the clock steps BETWEEN aperture samples, so one sweep is both the aperture and the exposure. Real depth of field and real motion blur in the same frame, at no extra cost over depth of field alone.\r\n"
  "By far the slowest thing here either way - every frame is a whole aperture sweep, so a shot measured in minutes elsewhere is measured in HOURS. Set the bokeh shape in ReShade's IGCS Connector panel." },

{ "RenderDofBokehSize", "Aperture", S_FLOAT, G_DOF, nullptr,
  "How wide the lens opens, in world units. The whole creative control: bigger means shallower focus and larger bokeh, and it costs nothing extra to render.\r\n"
  "What it costs is samples. The defocus disc is filled by discrete points, so a wide aperture at low bokeh quality shows each out-of-focus highlight as a ring of separate dots instead of a smooth circle. Open it up and raise quality together.\r\n"
  "The bokeh SHAPE - vertices, rounding, rotation, aberration, fringe - stays in ReShade's IGCS Connector panel, because it is chosen by looking at a live image.",
  false, 0.001, 4.0 },

{ "RenderDofQuality", "Bokeh quality", S_INT, G_DOF, nullptr,
  "Rings of samples across the aperture. The total sample count grows with it, and so does render time - close to proportionally.\r\n"
  "Set it by the blur you are asking for rather than by taste. A defocused highlight becomes exactly as many dots as there are samples, so a shot with small speculars out of focus needs far more than one without; smooth surfaces converge quickly, bright points are what force the number up.\r\n"
  "12 is a reasonable starting point. 29 is roughly 1200 samples and about a minute per frame.",
  false, 1, 60 },

{ "RenderDofAutofocus", "Autofocus", S_BOOL, G_DOF, nullptr,
  "Measure focus in the world every frame, at the centre of frame, so it follows the subject through the shot.\r\n"
  "The measurement is a ray fired into the scene - not a guess from the depth buffer - so it lands on the surface you are actually pointing at. And it is a DEPTH rather than a spot: everything the same distance from the camera comes out sharp too.\r\n"
  "Off hands focus to the markers. Manual focus is PER-MARKER and is set in the editor, not here: open a depth-of-field session, focus by eye, and press Copy to keyframe in the add-on's panel - or type into the marker's own Focus Distance row. Two different values across a shot give a focus pull.\r\n"
  "There is no global number to set. RenderDofFocusDelta in the ini is only the fallback for a marker that has nothing of its own, and the mod rescales it for you whenever the aperture changes - so a value typed in by hand does not stay put." },

// --- Image files -----------------------------------------------------------
{ "RenderJpeg", "Write JPEG instead of PNG", S_BOOL, G_FILES, nullptr,
  "PNG is lossless and the default. JPEG is smaller and faster to write, and lossy." },

{ "RenderQuality", "JPEG quality", S_INT, G_FILES, nullptr,
  "1 to 100. Only used when JPEG is on. Below 91 also subsamples chroma, which shows up on saturated edges long before the luma artefacts do.",
  false, 1, 100 },

{ "RenderKeepFrames", "Keep frames as well", S_BOOL, G_FILES, nullptr,
  "Video output only. Normally the frames are deleted as ffmpeg consumes them - turn this on to keep both the video and the image sequence.\r\n"
  "Costs disk: a sequence is far larger than the video made from it." },

// --- While rendering -------------------------------------------------------
{ "RenderHideHud", "Hide HUD and cursor", S_BOOL, G_SESSION, nullptr,
  "Hides the editor HUD and cursor for the duration and restores them afterwards." },

{ "ExportCloseWhenDone", "Return to menus when done", S_BOOL, G_SESSION, nullptr,
  "Go back to the editor menus once the render finishes, rather than staying in playback." },

// --- Output and encoding ---------------------------------------------------
{ "RenderOutputFolder", "Output folder", S_TEXT, G_ENCODE, nullptr,
  "OPTIONAL. Leave it empty and renders go to RockstarEditorPlus\\Captures\\ next to the .asi - fill it in only to put them somewhere else.\r\n"
  "Sequences are large, so another drive is worth it if the game is on a small one. An absolute path is what you want here; a bare folder name is taken relative to the game." },

{ "RenderVideoPreset", "Encoder preset", S_PRESET, G_ENCODE, nullptr,
  "WHICH CODEC THE VIDEO IS ENCODED WITH, by name, from the presets folder - twenty-one ship and the Encoder presets tab builds more.\r\n"
  "A preset supplies BOTH the ffmpeg arguments and the container, so while one is chosen the two rows below are ignored entirely and grey out to say so.\r\n"
  "Pick (none) only if you want to type ffmpeg arguments by hand." },

{ "RenderVideoArgs", "Arguments if no preset", S_TEXT, G_ENCODE, nullptr,
  "A FALLBACK, and only that. It is used when Encoder preset is (none), and ignored the moment a preset is chosen - which is why it greys out there.\r\n"
  "If you are not writing ffmpeg arguments by hand, this row is not the one to edit; pick a preset above instead.\r\n"
  "The container below has to be able to hold the codec named here, or ffmpeg refuses and the render produces nothing." },

{ "RenderVideoExt", "Container if no preset", S_TEXT, G_ENCODE, nullptr,
  "The other half of the fallback: the file extension, without the dot. Same rule as the row above - a chosen preset supplies its own and this is ignored.\r\n"
  "It must be able to hold the codec in the arguments above. mp4 will not take ProRes; mov and mkv take almost anything." },

{ "FfmpegPath", "ffmpeg path", S_TEXT, G_ENCODE, nullptr,
  "OPTIONAL. Empty is the right answer for almost everyone: the bundled RockstarEditorPlus\\ffmpeg.exe is used first, then one beside the game, then whatever is on PATH.\r\n"
  "The bundled copy wins on purpose, so renders encode the same way on every machine rather than depending on which ffmpeg somebody happens to have installed.\r\n"
  "Point this at another build only if you need a codec the bundled one lacks." },

// --- Audio -----------------------------------------------------------------
{ "RenderAudio", "Record audio", S_BOOL, G_AUDIO, nullptr,
  "One Export press does two passes: the project plays through once at normal speed to capture sound, then rewinds and renders the frames.\r\n"
  "Works in either output mode: Video muxes it in, Frames leaves it as audio.wav beside the sequence with the ffmpeg line to attach it in assemble.txt." },

{ "AudioFromFile", "Borrow audio from file", S_TEXT, G_AUDIO, nullptr,
  "OPTIONAL, and a fallback: it is only read when Record audio is OFF, which is why it greys out above.\r\n"
  "Point it at an existing file - normally a stock export of the same project - to use that file's audio track instead of recording one.\r\n"
  "Leave it empty and, with Record audio on, the sound is captured from the game during a first playthrough." },
};
static const int kSettingCount = (int)(sizeof(kSettings) / sizeof(kSettings[0]));

// ---------------------------------------------------------------------------
//  Encoder tables for the preset tab
// ---------------------------------------------------------------------------
//  A DROPDOWN ENTRY IS A LABEL AND THE COMPLETE ARGUMENTS IT CONTRIBUTES.
//
//  Complete, rather than a bare value the codec puts a flag in front of, which
//  is what this was. That shape could express exactly one rate control per
//  codec - "-crf" and a number - so a bitrate entry had to be smuggled in by
//  starting its value with a '-' and having buildArgs notice it. Every list
//  below carries its own flags, so one codec offers constant-quality, bitrate
//  and lossless in the same list with none of them a special case.
//
//  INDEX 0 IS THE RECOMMENDED CHOICE in every list. That is the whole default
//  mechanism - no per-codec "preferred" indices to keep in step with the lists
//  they point into.
//
//  Every string here was checked against the bundled ffmpeg rather than written
//  from memory: which encoders the build has, which of those actually open on
//  this machine, the -preset and -tune strings each accepts, and the pixel
//  formats each advertises. That matters more here than anywhere else in the
//  mod, because a bad preset fails invisibly - ffmpeg refuses, the render
//  produces no video, and the reason is a line of stderr nobody ever sees.
// ---------------------------------------------------------------------------
struct Opt { const char* label; const char* args; };

#define OPT_END { nullptr, nullptr }

// --- speed ladders ---------------------------------------------------------
static const Opt kSpeedX26x[] = {
    { "slow - the default here",   "-preset slow" },
    { "medium - ffmpeg default",   "-preset medium" },
    { "slower",                    "-preset slower" },
    { "veryslow",                  "-preset veryslow" },
    { "placebo - not worth it",    "-preset placebo" },
    { "fast",                      "-preset fast" },
    { "faster",                    "-preset faster" },
    { "veryfast",                  "-preset veryfast" },
    { "superfast",                 "-preset superfast" },
    { "ultrafast - preview only",  "-preset ultrafast" },
    OPT_END };

static const Opt kSpeedNvenc[] = {
    { "p7 - slowest, best",        "-preset p7" },
    { "p6",                        "-preset p6" },
    { "p5",                        "-preset p5" },
    { "p4 - balanced",             "-preset p4" },
    { "p3",                        "-preset p3" },
    { "p2",                        "-preset p2" },
    { "p1 - fastest",              "-preset p1" },
    OPT_END };

static const Opt kSpeedAom[] = {
    { "cpu-used 4 - usable",       "-cpu-used 4" },
    { "cpu-used 6",                "-cpu-used 6" },
    { "cpu-used 8 - fastest",      "-cpu-used 8" },
    { "cpu-used 2 - slow",         "-cpu-used 2" },
    { "cpu-used 0 - glacial",      "-cpu-used 0" },
    OPT_END };

static const Opt kSpeedVp9[] = {
    { "good",                      "-deadline good -cpu-used 2" },
    { "good, faster",              "-deadline good -cpu-used 4" },
    { "best - slow",               "-deadline best" },
    { "realtime",                  "-deadline realtime -cpu-used 5" },
    OPT_END };

// --- tunes -----------------------------------------------------------------
static const Opt kTuneX264[] = {
    { "(none)",                    "" },
    { "film - live action",        "-tune film" },
    { "animation",                 "-tune animation" },
    { "grain - preserve noise",    "-tune grain" },
    { "stillimage",                "-tune stillimage" },
    { "fastdecode",                "-tune fastdecode" },
    OPT_END };

static const Opt kTuneX265[] = {
    { "(none)",                    "" },
    { "grain - preserve noise",    "-tune grain" },
    { "animation",                 "-tune animation" },
    { "fastdecode",                "-tune fastdecode" },
    OPT_END };

static const Opt kTuneNvenc[] = {
    { "hq - quality",              "-tune hq" },
    { "ll - low latency",          "-tune ll" },
    { "ull - ultra low latency",   "-tune ull" },
    OPT_END };

// --- rate control ----------------------------------------------------------
static const Opt kRateX264[] = {
    { "CRF 16 - visually lossless","-crf 16" },
    { "CRF 18 - high",             "-crf 18" },
    { "CRF 20 - good",             "-crf 20" },
    { "CRF 23 - ffmpeg default",   "-crf 23" },
    { "CRF 28 - small",            "-crf 28" },
    { "Bitrate 150 Mbps",          "-b:v 150M" },
    { "Bitrate 100 Mbps",          "-b:v 100M" },
    { "Bitrate 50 Mbps",           "-b:v 50M" },
    { "Bitrate 25 Mbps",           "-b:v 25M" },
    { "Bitrate 12 Mbps",           "-b:v 12M" },
    OPT_END };

static const Opt kRateX265[] = {
    { "CRF 18 - visually lossless","-crf 18" },
    { "CRF 20 - high",             "-crf 20" },
    { "CRF 23 - good",             "-crf 23" },
    { "CRF 28 - ffmpeg default",   "-crf 28" },
    { "Bitrate 100 Mbps",          "-b:v 100M" },
    { "Bitrate 50 Mbps",           "-b:v 50M" },
    { "Bitrate 25 Mbps",           "-b:v 25M" },
    { "Bitrate 12 Mbps",           "-b:v 12M" },
    OPT_END };

// AV1 and VP9 want -b:v 0 alongside -crf or the CRF is treated as a cap on a
// bitrate target rather than the target itself.
static const Opt kRateAv1[] = {
    { "CRF 20 - high",             "-crf 20 -b:v 0" },
    { "CRF 25",                    "-crf 25 -b:v 0" },
    { "CRF 30 - good",             "-crf 30 -b:v 0" },
    { "CRF 40 - small",            "-crf 40 -b:v 0" },
    { "Bitrate 50 Mbps",           "-b:v 50M" },
    { "Bitrate 25 Mbps",           "-b:v 25M" },
    { "Bitrate 12 Mbps",           "-b:v 12M" },
    OPT_END };

static const Opt kRateVp9[] = {
    { "CRF 20 - high",             "-crf 20 -b:v 0" },
    { "CRF 28 - good",             "-crf 28 -b:v 0" },
    { "CRF 36 - small",            "-crf 36 -b:v 0" },
    { "Bitrate 50 Mbps",           "-b:v 50M" },
    { "Bitrate 25 Mbps",           "-b:v 25M" },
    OPT_END };

// NVENC constant quality needs the VBR rate controller with the bitrate target
// cleared; -cq on its own is only a ceiling and the encoder still chases a
// default bitrate.
static const Opt kRateNvenc[] = {
    { "CQ 19 - visually lossless", "-rc vbr -cq 19 -b:v 0" },
    { "CQ 22 - high",              "-rc vbr -cq 22 -b:v 0" },
    { "CQ 25 - good",              "-rc vbr -cq 25 -b:v 0" },
    { "CQ 30 - small",             "-rc vbr -cq 30 -b:v 0" },
    { "Bitrate 150 Mbps",          "-rc vbr -b:v 150M" },
    { "Bitrate 100 Mbps",          "-rc vbr -b:v 100M" },
    { "Bitrate 50 Mbps",           "-rc vbr -b:v 50M" },
    { "Bitrate 25 Mbps",           "-rc vbr -b:v 25M" },
    OPT_END };

// PROFILE AND PIXEL FORMAT ARE ONE CHOICE for ProRes and DNxHR, so they are one
// entry. Neither encoder reconciles a mismatched pair - both refuse at init with
// the reason on stderr, which for a render means no video and no visible cause.
//
// This used to be two dropdowns with a fixup in buildArgs that quietly rewrote
// the pixel format for ProRes. That worked, but it only ever covered ProRes;
// DNxHR was added later and ten of its fifteen combinations were dead on
// arrival. Folding the format into the profile makes the invariant a property
// of the table rather than of a function somebody has to remember to extend.
static const Opt kRateProRes[] = {
    { "422 HQ - the usual choice", "-profile:v hq -pix_fmt yuv422p10le" },
    { "422",                       "-profile:v standard -pix_fmt yuv422p10le" },
    { "422 LT",                    "-profile:v lt -pix_fmt yuv422p10le" },
    { "422 Proxy",                 "-profile:v proxy -pix_fmt yuv422p10le" },
    { "4444 - full colour",        "-profile:v 4444 -pix_fmt yuv444p10le" },
    { "4444 XQ - full colour",     "-profile:v 4444xq -pix_fmt yuv444p10le" },
    OPT_END };

static const Opt kRateDnx[] = {
    { "DNxHR HQ - 8-bit 4:2:2",    "-profile:v dnxhr_hq -pix_fmt yuv422p" },
    { "DNxHR HQX - 10-bit 4:2:2",  "-profile:v dnxhr_hqx -pix_fmt yuv422p10le" },
    { "DNxHR 444 - 10-bit 4:4:4",  "-profile:v dnxhr_444 -pix_fmt yuv444p10le" },
    { "DNxHR SQ - 8-bit 4:2:2",    "-profile:v dnxhr_sq -pix_fmt yuv422p" },
    { "DNxHR LB - 8-bit, proxy",   "-profile:v dnxhr_lb -pix_fmt yuv422p" },
    OPT_END };

static const Opt kRateCfhd[] = {
    { "film3+ - highest",          "-quality film3+" },
    { "film2",                     "-quality film2" },
    { "film1",                     "-quality film1" },
    { "high",                      "-quality high" },
    { "medium",                    "-quality medium" },
    { "low",                       "-quality low" },
    OPT_END };

static const Opt kRateMjpeg[] = {
    { "q 2 - near lossless",       "-q:v 2" },
    { "q 3 - high",                "-q:v 3" },
    { "q 5 - good",                "-q:v 5" },
    { "q 8 - small",               "-q:v 8" },
    OPT_END };

static const Opt kRateNone[] = {
    { "Mathematically lossless",   "" },
    OPT_END };

static const Opt kRateVc2[] = {
    { "Lossless",                  "-qp 0" },
    { "Bitrate 200 Mbps",          "-b:v 200M" },
    { "Bitrate 100 Mbps",          "-b:v 100M" },
    OPT_END };

static const Opt kRateGif[] = {
    { "Standard palette",          "" },
    OPT_END };

static const Opt kRateWebp[] = {
    { "Lossy q 80",                "-lossless 0 -quality 80" },
    { "Lossy q 90",                "-lossless 0 -quality 90" },
    { "Lossless",                  "-lossless 1" },
    OPT_END };

// --- prediction, for the fast lossless pair --------------------------------
static const Opt kPredUt[] = {
    { "median - smallest",         "-pred median" },
    { "gradient",                  "-pred gradient" },
    { "left - fastest",            "-pred left" },
    OPT_END };

// --- pixel formats ---------------------------------------------------------
// Curated. Every encoder advertises far more than this - libx265 lists twenty -
// and offering all of them is noise that also lets you pick something the
// container will not carry.
static const Opt kPixSdr8[] = {
    { "4:2:0 8-bit - compatible",  "-pix_fmt yuv420p" },
    { "4:2:2 8-bit",               "-pix_fmt yuv422p" },
    { "4:4:4 8-bit - full colour", "-pix_fmt yuv444p" },
    OPT_END };

static const Opt kPixSdr8And10[] = {
    { "4:2:0 8-bit - compatible",  "-pix_fmt yuv420p" },
    { "4:2:0 10-bit",              "-pix_fmt yuv420p10le" },
    { "4:2:2 8-bit",               "-pix_fmt yuv422p" },
    { "4:4:4 8-bit - full colour", "-pix_fmt yuv444p" },
    { "4:4:4 10-bit",              "-pix_fmt yuv444p10le" },
    OPT_END };

static const Opt kPixNvenc[] = {
    { "4:2:0 8-bit - compatible",  "-pix_fmt yuv420p" },
    { "4:2:0 10-bit",              "-pix_fmt p010le" },
    { "4:4:4 8-bit - full colour", "-pix_fmt yuv444p" },
    OPT_END };

static const Opt kPixNvencAv1[] = {
    { "4:2:0 8-bit - compatible",  "-pix_fmt yuv420p" },
    { "4:2:0 10-bit",              "-pix_fmt p010le" },
    OPT_END };

// ProRes and DNxHR carry their format in the profile, so there is nothing left
// to choose here - but the control still needs something to show.
static const Opt kPixFromProfile[] = {
    { "(set by the profile)",      "" },
    OPT_END };

static const Opt kPixCfhd[] = {
    { "4:2:2 10-bit",              "-pix_fmt yuv422p10le" },
    { "RGB 12-bit",                "-pix_fmt gbrp12le" },
    OPT_END };

static const Opt kPixFastLossless[] = {
    { "RGB - no conversion",       "-pix_fmt gbrp" },
    { "4:4:4 - full colour",       "-pix_fmt yuv444p" },
    { "4:2:2",                     "-pix_fmt yuv422p" },
    { "4:2:0 - smallest",          "-pix_fmt yuv420p" },
    OPT_END };

static const Opt kPixFfv1[] = {
    { "4:4:4 8-bit",               "-pix_fmt yuv444p" },
    { "4:4:4 10-bit",              "-pix_fmt yuv444p10le" },
    { "RGB 8-bit",                 "-pix_fmt gbrp" },
    { "4:2:0 8-bit - smallest",    "-pix_fmt yuv420p" },
    OPT_END };

static const Opt kPixX264Lossless[] = {
    { "4:4:4 - full colour",       "-pix_fmt yuv444p" },
    { "4:2:0 - smaller, lossy colour", "-pix_fmt yuv420p" },
    OPT_END };

static const Opt kPixHuffyuv[] = {
    { "RGB",                       "-pix_fmt rgb24" },
    { "4:2:2",                     "-pix_fmt yuv422p" },
    OPT_END };

static const Opt kPixQtrle[] = {
    { "RGB",                       "-pix_fmt rgb24" },
    OPT_END };

static const Opt kPixMjpeg[] = {
    { "4:4:4 - full colour",       "-pix_fmt yuvj444p" },
    { "4:2:2",                     "-pix_fmt yuvj422p" },
    { "4:2:0 - smallest",          "-pix_fmt yuvj420p" },
    OPT_END };

static const Opt kPixVc2[] = {
    { "4:2:2 10-bit",              "-pix_fmt yuv422p10le" },
    { "4:4:4 10-bit",              "-pix_fmt yuv444p10le" },
    { "4:2:0 8-bit",               "-pix_fmt yuv420p" },
    OPT_END };

static const Opt kPixV210[] = {
    { "4:2:2 10-bit",              "-pix_fmt yuv422p10le" },
    OPT_END };

static const Opt kPixR210[] = {
    { "RGB 10-bit",                "-pix_fmt gbrp10le" },
    OPT_END };

static const Opt kPixGif[] = {
    { "256-colour palette",        "" },
    OPT_END };

static const Opt kPixWebp[] = {
    { "RGBA",                      "-pix_fmt bgra" },
    { "4:2:0",                     "-pix_fmt yuv420p" },
    OPT_END };

static const Opt kPixApng[] = {
    { "RGB",                       "-pix_fmt rgb24" },
    { "RGBA",                      "-pix_fmt rgba" },
    OPT_END };

// --- containers ------------------------------------------------------------
static const char* const kExtMp4[]  = { "mp4", "mkv", "mov", nullptr };
static const char* const kExtMkv[]  = { "mkv", nullptr };
static const char* const kExtMkvAvi[] = { "mkv", "avi", nullptr };
static const char* const kExtMov[]  = { "mov", "mkv", nullptr };
static const char* const kExtMovOnly[] = { "mov", nullptr };
static const char* const kExtAvi[]  = { "avi", "mkv", nullptr };
static const char* const kExtWebm[] = { "webm", "mkv", nullptr };
static const char* const kExtGif[]  = { "gif", nullptr };
static const char* const kExtWebp[] = { "webp", nullptr };
static const char* const kExtApng[] = { "apng", nullptr };

// ---------------------------------------------------------------------------
//  The codecs themselves
// ---------------------------------------------------------------------------
//  `group` only orders and prefixes the dropdown. `encoder` may carry fixed
//  arguments that are part of what the entry IS rather than a choice - x265
//  lossless is the codec plus one x265-param, not a rate control.
//
//  `colour` marks the codecs where BT.709 tagging is worth writing: an editor
//  that reads an untagged file guesses, and guesses 601 for standard
//  definition often enough to shift every hue slightly. It is off for the
//  share formats, which have nowhere to put it.
struct Codec
{
    const char* name;
    const char* group;
    const char* encoder;   // -c:v value plus any fixed arguments
    const Opt*  rate;
    const Opt*  speed;     // may be nullptr
    const Opt*  tune;      // may be nullptr
    const Opt*  pix;
    const char* const* exts;
    bool        colour;    // offer BT.709 tagging
    const char* note;
};

static const Codec kCodecs[] =
{
// --- Delivery --------------------------------------------------------------
{ "H.264 (x264)", "Delivery", "libx264",
  kRateX264, kSpeedX26x, kTuneX264, kPixSdr8And10, kExtMp4, true,
  "The safe default. Plays and edits anywhere, and every phone decodes it in hardware.\r\n"
  "CRF targets a QUALITY and lets the size land where it lands - the right choice when the file is going into an editor. A bitrate targets a SIZE and lets quality vary, which is what a delivery spec or an upload limit asks for.\r\n"
  "4:2:0 is what players expect; 4:4:4 keeps saturated text and UI intact but many players will not touch it." },

{ "H.265 / HEVC (x265)", "Delivery", "libx265",
  kRateX265, kSpeedX26x, kTuneX265, kPixSdr8And10, kExtMp4, true,
  "Smaller than H.264 at the same quality, and slower to encode - roughly half the bitrate for a comparable picture.\r\n"
  "Playback is less universal: fine in modern players and editors, awkward in older ones and on some web platforms." },

{ "AV1 (libaom)", "Delivery", "libaom-av1",
  kRateAv1, kSpeedAom, nullptr, kPixSdr8And10, kExtMkv, true,
  "The best compression here by some way, and by far the slowest software encoder - cpu-used 0 on a long clip is an overnight job.\r\n"
  "Made for uploading. YouTube and the rest re-encode anyway, so the win is bandwidth on the way up, not final quality.\r\n"
  "If you have an RTX 40-series or newer, the NVENC AV1 entry does the same job in real time at a small quality cost." },

{ "VP9 (libvpx)", "Delivery", "libvpx-vp9",
  kRateVp9, kSpeedVp9, nullptr, kPixSdr8And10, kExtWebm, true,
  "AV1's predecessor and the safe WebM choice. Slower than H.264, smaller than it, and decoded natively by every browser.\r\n"
  "Pick this over AV1 when the file has to play in something old, and over H.264 when it has to be a .webm." },

{ "H.264 (NVENC)", "Delivery", "h264_nvenc",
  kRateNvenc, kSpeedNvenc, kTuneNvenc, kPixNvenc, kExtMp4, true,
  "GPU encode. Many times faster than x264 and noticeably less efficient at the same quality - the file is bigger for the same look.\r\n"
  "Worth it when the render itself is the slow part, which with motion blur it always is." },

{ "HEVC (NVENC)", "Delivery", "hevc_nvenc",
  kRateNvenc, kSpeedNvenc, kTuneNvenc, kPixNvenc, kExtMp4, true,
  "GPU HEVC. The usual pick for long renders: much faster than x265 and small enough that the efficiency loss rarely matters.\r\n"
  "10-bit costs nothing extra on the GPU and removes banding from skies and smoke, which is where 8-bit shows first." },

{ "AV1 (NVENC)", "Delivery", "av1_nvenc",
  kRateNvenc, kSpeedNvenc, nullptr, kPixNvencAv1, kExtMkv, true,
  "GPU AV1. Needs an RTX 40-series or newer - the availability line above says whether this machine has it.\r\n"
  "Close to libaom's efficiency at a fraction of the time. The best upload-target choice if your card can do it." },

// --- Editing intermediates -------------------------------------------------
{ "ProRes", "Editing", "prores_ks",
  kRateProRes, nullptr, nullptr, kPixFromProfile, kExtMov, true,
  "The editing-timeline standard. Large files that scrub instantly and grade well.\r\n"
  "The PROFILE and the pixel format are not independent: 422 profiles carry 4:2:2 and the 4444 profiles carry 4:4:4. A mismatched pair is corrected for you, because prores_ks refuses to start on one rather than fixing it - which would mean a render that produces no video at all.\r\n"
  "4:2:2 halves colour resolution and shreds saturated text; pick 4444 if anything on screen is a UI element." },

{ "DNxHR", "Editing", "dnxhd",
  kRateDnx, nullptr, nullptr, kPixFromProfile, kExtMov, true,
  "Avid's intermediate, and what Resolve and Media Composer prefer. Same role as ProRes, better supported on Windows.\r\n"
  "The profile fixes the data rate rather than a quality target, so file size is predictable from length alone. HQX and 444 are the 10-bit ones." },

{ "GoPro CineForm", "Editing", "cfhd",
  kRateCfhd, nullptr, nullptr, kPixCfhd, kExtMov, true,
  "A wavelet intermediate - visually lossless at film3+, smaller than ProRes for the same quality, and it scrubs as well.\r\n"
  "Less universally read than ProRes or DNxHR, so it is the choice when you control both ends." },

{ "MJPEG", "Editing", "mjpeg",
  kRateMjpeg, nullptr, nullptr, kPixMjpeg, kExtMov, false,
  "Every frame a JPEG. Ancient, universally readable, and much larger than anything modern at the same quality.\r\n"
  "Useful as a lowest-common-denominator intermediate when something refuses to open everything else." },

// --- Lossless --------------------------------------------------------------
{ "Ut Video", "Lossless", "utvideo",
  kRateNone, kPredUt, nullptr, kPixFastLossless, kExtMkvAvi, false,
  "The practical replacement for Lagarith, and faster than it. Bit-exact, and there is a free VfW codec for Windows so VirtualDub, AviUtl, Vegas and Premiere read it straight off the timeline.\r\n"
  "Compresses far less than FFV1 or x264 lossless - roughly 50% larger than FFV1 - which is the trade for decoding fast enough to scrub.\r\n"
  "RGB avoids a colour conversion entirely, since the renderer hands over RGB frames." },

{ "MagicYUV", "Lossless", "magicyuv",
  kRateNone, kPredUt, nullptr, kPixFastLossless, kExtMkvAvi, false,
  "The same idea as Ut Video and about the same size, with a Windows VfW codec of its own. Pick whichever your editor already has installed." },

{ "FFV1", "Lossless", "ffv1 -level 3",
  kRateNone, nullptr, nullptr, kPixFfv1, kExtMkvAvi, false,
  "Archival lossless, and about a third smaller than Ut Video for the same bit-exact result.\r\n"
  "Slower to decode, so it scrubs badly on a timeline - this is a storage format, not an editing one. It is what film archives use." },

{ "H.264 lossless (x264)", "Lossless", "libx264 -qp 0",
  kRateNone, kSpeedX26x, nullptr, kPixX264Lossless, kExtMkv, false,
  "The smallest lossless option here by a wide margin - roughly a quarter the size of Ut Video - because it is a real video codec doing motion compensation rather than a per-frame packer.\r\n"
  "Slow to encode and slow to scrub. 4:4:4 keeps colour intact; 4:2:0 is smaller but discards chroma before the encoder sees it, which is no longer lossless in any sense that matters." },

{ "FFVHuff", "Lossless", "ffvhuff",
  kRateNone, nullptr, nullptr, kPixFastLossless, kExtMkvAvi, false,
  "Huffyuv with better prediction. Fast, and the largest files of any lossless option here.\r\n"
  "Kept for compatibility with old tooling; Ut Video beats it on every axis." },

{ "HuffYUV", "Lossless", "huffyuv",
  kRateNone, nullptr, nullptr, kPixHuffyuv, kExtAvi, false,
  "The original fast lossless AVI codec. Enormous by modern standards.\r\n"
  "Only worth choosing when something specifically demands HuffYUV in an AVI." },

{ "QuickTime RLE", "Lossless", "qtrle",
  kRateNone, nullptr, nullptr, kPixQtrle, kExtMovOnly, false,
  "Lossless MOV that every editor on both platforms reads without installing anything. Enormous files - roughly twice FFV1.\r\n"
  "The safe answer when a preset has to work on a machine you cannot install codecs on." },

{ "SMPTE VC-2 (Dirac)", "Lossless", "vc2",
  kRateVc2, nullptr, nullptr, kPixVc2, kExtMkv, false,
  "A broadcast intra codec, losslessly at qp 0 or at a fixed bitrate. Rarely what you want unless a broadcast workflow asked for it." },

// --- Uncompressed ----------------------------------------------------------
{ "Uncompressed 4:2:2 10-bit (v210)", "Uncompressed", "v210",
  kRateNone, nullptr, nullptr, kPixV210, kExtMovOnly, true,
  "No compression at all - about 1 GB per 10 seconds at 1080p.\r\n"
  "Only for handing frames to something that demands raw v210. Any lossless codec above is bit-identical and a fraction of the size." },

{ "Uncompressed RGB 10-bit (r210)", "Uncompressed", "r210",
  kRateNone, nullptr, nullptr, kPixR210, kExtMovOnly, true,
  "Raw 10-bit RGB. Same caveat as v210, and larger still. Use a lossless codec unless something specifically requires this." },

// --- Share -----------------------------------------------------------------
{ "GIF", "Share", "gif",
  kRateGif, nullptr, nullptr, kPixGif, kExtGif, false,
  "256 colours, no audio, and large for what it is. Keep the clip short and the frame rate low.\r\n"
  "For anything longer than a few seconds a small MP4 looks better and weighs less." },

{ "WebP (animated)", "Share", "libwebp_anim",
  kRateWebp, nullptr, nullptr, kPixWebp, kExtWebp, false,
  "A far better GIF: full colour, alpha, and a fraction of the size. Every current browser shows it inline.\r\n"
  "Lossless mode is genuinely lossless but only sensible for very short clips." },

{ "APNG", "Share", "apng",
  kRateNone, nullptr, nullptr, kPixApng, kExtApng, false,
  "Lossless animated PNG. Full colour and alpha, universally supported in browsers, and very large.\r\n"
  "Worth it for a short clip that must stay pixel-exact and must play inline." },
};
static const int kCodecCount = (int)(sizeof(kCodecs) / sizeof(kCodecs[0]));


// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum { ID_TAB = 900, ID_SAVE, ID_RELOAD, ID_BROWSE, ID_HELP, ID_PATH,
       ID_PLIST = 1200, ID_PNAME, ID_PCODEC, ID_PRATE, ID_PEXT, ID_PPIX,
       ID_PARGS, ID_PNOTE, ID_PSAVE, ID_PDEL,
       ID_PSPEED, ID_PTUNE, ID_PCOLOUR, ID_PEXTRA, ID_PTEST, ID_PSTATUS,
       ID_SETTING = 1400 };

static HWND g_main, g_tab, g_help, g_pathLabel, g_tip;
static HWND g_ctl[kSettingCount];          // one control per setting
static HWND g_ctlLabel[kSettingCount];
static HWND g_box[G_COUNT];                // the group frames
static HWND g_plist, g_pname, g_pcodec, g_prate, g_pext, g_ppix, g_pargs, g_pnote;
static HWND g_pspeed, g_ptune, g_pcolour, g_pextra, g_pstatus;
static std::string g_ffmpegPath;
static std::vector<HWND> g_presetTabCtls;
static std::string g_iniPath, g_presetDir;
static std::vector<std::string> g_lines;   // Render.ini, verbatim
static bool g_dirty = false;
static bool g_loading = false;             // suppress dirty while filling controls

// ---------------------------------------------------------------------------
//  Paths
// ---------------------------------------------------------------------------
static std::string exeDir()
{
    char p[MAX_PATH]{};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    if (char* s = strrchr(p, '\\')) *(s + 1) = '\0';
    return p;
}

static bool fileExists(const std::string& p)
{ return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The tool may sit beside the .asi, inside RockstarEditorPlus\, or one level up
// if someone moved it. Try each before giving up.
static void resolvePaths()
{
    const std::string b = exeDir();
    const char* roots[] = { "RockstarEditorPlus\\", "", "..\\RockstarEditorPlus\\", "..\\" };
    for (const char* r : roots)
    {
        if (fileExists(b + r + "Render.ini"))
        {
            g_iniPath   = b + r + "Render.ini";
            g_presetDir = b + r + "presets\\";
            return;
        }
    }
    g_iniPath   = b + "RockstarEditorPlus\\Render.ini";
    g_presetDir = b + "RockstarEditorPlus\\presets\\";
}

// ---------------------------------------------------------------------------
//  Line-preserving ini
// ---------------------------------------------------------------------------
static void trimR(std::string& s)
{ while (!s.empty() && (s.back()=='\r'||s.back()=='\n'||s.back()==' '||s.back()=='\t')) s.pop_back(); }

static void loadIni()
{
    g_lines.clear();
    HANDLE h = CreateFileA(g_iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string all;
    char buf[4096]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) all.append(buf, got);
    CloseHandle(h);

    size_t start = 0;
    while (start <= all.size())
    {
        size_t nl = all.find('\n', start);
        if (nl == std::string::npos) { g_lines.push_back(all.substr(start)); break; }
        std::string line = all.substr(start, nl - start);
        trimR(line);
        g_lines.push_back(line);
        start = nl + 1;
    }
}

// Value of a key, ignoring any trailing "; comment".
static std::string iniGet(const char* key)
{
    const size_t klen = strlen(key);
    for (const std::string& l : g_lines)
    {
        if (l.size() <= klen || _strnicmp(l.c_str(), key, klen) != 0) continue;
        size_t i = klen;
        while (i < l.size() && (l[i]==' '||l[i]=='\t')) ++i;
        if (i >= l.size() || l[i] != '=') continue;
        std::string v = l.substr(i + 1);
        const size_t c = v.find(';');
        if (c != std::string::npos) v = v.substr(0, c);
        while (!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
        trimR(v);
        return v;
    }
    return "";
}

// Replace only the value, keeping the key's own spelling and any trailing
// comment exactly as it was. Appends the key if the file does not have it.
static void iniSet(const char* key, const std::string& val)
{
    const size_t klen = strlen(key);
    for (std::string& l : g_lines)
    {
        if (l.size() <= klen || _strnicmp(l.c_str(), key, klen) != 0) continue;
        size_t i = klen;
        while (i < l.size() && (l[i]==' '||l[i]=='\t')) ++i;
        if (i >= l.size() || l[i] != '=') continue;

        const std::string head = l.substr(0, i + 1);
        std::string tail;
        const size_t c = l.find(';', i);
        if (c != std::string::npos)
        {
            // Keep the comment where it sat, padded so the file stays aligned.
            const std::string comment = l.substr(c);
            std::string pad = " ";
            const size_t want = head.size() + val.size();
            if (want < 38) pad = std::string(38 - want, ' ');
            tail = pad + comment;
        }
        l = head + val + tail;
        return;
    }
    g_lines.push_back(std::string(key) + "=" + val);
}

static bool saveIni()
{
    std::string out;
    for (size_t i = 0; i < g_lines.size(); ++i)
    {
        out += g_lines[i];
        if (i + 1 < g_lines.size()) out += "\r\n";
    }
    HANDLE h = CreateFileA(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
static std::string editText(HWND h)
{ char b[1024]{}; GetWindowTextA(h, b, sizeof(b)); return b; }

static std::string comboText(HWND h)
{
    const int i = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
    if (i < 0) return "";
    char b[128]{}; SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b); return b;
}

// What an EMPTY box actually does.
//
// Three of the text rows default to something useful when left blank, and blank
// reads as "unset" rather than "the default" unless the default is written where
// the value would go. Putting it in the help text alone does not work: the help
// only appears once you hover or click the row, which is after you have already
// decided whether the row matters.
static const struct { const char* key; const wchar_t* cue; } kCues[] = {
    { "RenderOutputFolder", L"RockstarEditorPlus\\Captures\\ beside the .asi" },
    { "FfmpegPath",         L"the bundled ffmpeg.exe, then beside the game, then PATH" },
    { "AudioFromFile",      L"recorded from the game, while Record audio is on" },
};

static void setCue(HWND edit, const char* key)
{
    for (const auto& c : kCues)
        if (_stricmp(c.key, key) == 0)
        {
            // Unicode-only message, so it is sent explicitly wide even though
            // the rest of this file is ANSI.
            SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)c.cue);
            return;
        }
}

static void addTip(HWND ctl, const char* text)
{
    TOOLINFOA ti{};
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = GetParent(ctl);
    ti.uId      = (UINT_PTR)ctl;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void fillPresetCombo(HWND h)
{
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)"(none - use the two rows below)");
    WIN32_FIND_DATAA fd{};
    HANDLE f = FindFirstFileA((g_presetDir + "*.ini").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        const size_t d = n.rfind('.');
        if (d != std::string::npos) n = n.substr(0, d);
        SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

// ---------------------------------------------------------------------------
//  Reading the LIVE controls
// ---------------------------------------------------------------------------
//  Relevance is decided from what is on screen, not from what is in the file -
//  otherwise ticking Depth of field would leave Aperture greyed until a save
//  and a reload, which reads as the tick not having worked.
static int idxOf(const char* key)
{
    for (int i = 0; i < kSettingCount; ++i)
        if (_stricmp(kSettings[i].key, key) == 0) return i;
    return -1;
}

static bool uiBool(const char* key)
{
    const int i = idxOf(key);
    return i >= 0 && SendMessageA(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int uiSel(const char* key)
{
    const int i = idxOf(key);
    return i < 0 ? 0 : (int)SendMessageA(g_ctl[i], CB_GETCURSEL, 0, 0);
}

static double uiNum(const char* key)
{
    const int i = idxOf(key);
    return i < 0 ? 0.0 : atof(editText(g_ctl[i]).c_str());
}

// ---------------------------------------------------------------------------
//  Which settings currently mean anything
// ---------------------------------------------------------------------------
//  The first half MIRRORS rowEnabled() in src\ui\exportmenu.cpp. That function
//  is the authority - it was written against what the renderer actually reads,
//  with the reasoning recorded next to it - so this copies its rules and their
//  order rather than deriving them again and drifting.
//
//  The second half is this tool's own, for keys the in-editor menu has no row
//  for. Each of those comes from a stated fact elsewhere in the codebase, cited
//  on the line, so none of it is a guess about what "probably" does nothing.
//
//  ONE RULE IS DELIBERATELY NOT COPIED: the menu greys everything while
//  EnableRenderer is off, and this does not.
//
//  The two are answering different questions. The menu is a live control
//  surface, so "off" there means "this will not affect the render you are about
//  to start" - true, and worth showing. This edits a FILE, which outlives the
//  session, and the renderer ships disabled: copying the rule meant every
//  control was grey the first time anyone opened the tool, which reads as
//  broken rather than as informative, and stops you setting a shot up before
//  turning the renderer on.
//
//  What is left greys on the CONTENTS OF THE FILE rather than on the moment -
//  JPEG quality with PNG selected, arguments with a preset written over them -
//  and those stay true whenever the render eventually runs.
static bool settingEnabled(int i)
{
    const char* k = kSettings[i].key;
    auto isKey = [&](const char* n) { return _stricmp(k, n) == 0; };

    if (isKey("EnableRenderer")) return true;

    const bool dof     = uiBool("RenderDepthOfField");
    const bool video   = uiSel("RenderMode") == 0;          // Video | Frames
    const bool walking = uiSel("RenderCaptureMode") == 0;   // Walking | Sliding
    const double samples = uiNum("RenderSamples");

    // --- mirrors exportmenu.cpp rowEnabled() -------------------------------
    if (dof)
    {
        if (isKey("RenderHighlightBoost")) return false;
        if (isKey("RenderShutter"))        return true;
    }
    if ((isKey("RenderShutter") || isKey("RenderHighlightBoost")) && samples <= 1)
        return false;
    if (!dof && (isKey("RenderDofBokehSize") || isKey("RenderDofQuality") ||
                 isKey("RenderDofAutofocus")))
        return false;
    if (isKey("RenderSamples") && dof) return false;

    // --- this tool's own ----------------------------------------------------
    // render.cpp only reaches settleSubFrames from the walking sub-sample step,
    // and one sample never takes it. settleFrames is deliberately NOT gated the
    // same way - it is also the post-load resettle wait, in both modes.
    if (isKey("RenderSettleSubFrames")) return walking && samples > 1;

    // "Only used when JPEG is on."
    if (isKey("RenderQuality")) return uiBool("RenderJpeg");

    // Everything below here is the encoder, which only the Video path runs.
    if (isKey("RenderKeepFrames") || isKey("RenderVideoPreset") || isKey("FfmpegPath"))
        return video;

    // Render.ini: "IGNORED while a preset is named above."
    if (isKey("RenderVideoArgs") || isKey("RenderVideoExt"))
        return video && uiSel("RenderVideoPreset") <= 0;

    // "Ignored while Record audio is on."
    if (isKey("AudioFromFile")) return !uiBool("RenderAudio");

    return true;
}

static void refreshEnabled()
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        const BOOL on = settingEnabled(i) ? TRUE : FALSE;
        EnableWindow(g_ctl[i], on);
        EnableWindow(g_ctlLabel[i], on);
    }
}

static void setDirty(bool d)
{
    if (g_dirty == d) return;
    g_dirty = d;
    SetWindowTextA(g_main, d ? "Rockstar Editor+ - Render Settings *"
                             : "Rockstar Editor+ - Render Settings");
}

// ---------------------------------------------------------------------------
//  Asking ffmpeg
// ---------------------------------------------------------------------------
//  The tab's promise is that a broken combination cannot be built, and only
//  ffmpeg can settle what is broken. Two things are otherwise invisible from
//  here: an encoder the build does not have, and an encoder the build HAS but
//  this machine cannot open - every QSV and AMF entry is present in the bundled
//  ffmpeg and none of them work without the matching hardware. A preset built
//  on one of those produces no video and a stderr line nobody sees.
//
//  So the codec list is filtered by what the build reports, and the selected
//  combination is test-encoded on demand.
static std::string findFfmpeg()
{
    // Beside the ini we are editing first - that is the copy the mod itself
    // uses, so it is the one whose answers apply.
    const size_t slash = g_iniPath.find_last_of('\\');
    if (slash != std::string::npos)
    {
        const std::string beside = g_iniPath.substr(0, slash + 1) + "ffmpeg.exe";
        if (fileExists(beside)) return beside;
    }
    const std::string b = exeDir() + "ffmpeg.exe";
    if (fileExists(b)) return b;

    char found[MAX_PATH]{};
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr))
        return found;
    return "";
}

// Runs ffmpeg and returns its exit code, with the tail of stderr in `err`.
static bool runFfmpeg(const std::string& args, std::string& out, DWORD waitMs)
{
    out.clear();
    if (g_ffmpegPath.empty()) return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + g_ffmpegPath + "\" " + args;
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) { CloseHandle(rd); return false; }

    // Drain as it runs; a full pipe would otherwise block the child forever.
    char buf[1024]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, waitMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == STILL_ACTIVE) { TerminateProcess(pi.hProcess, 1); code = 1; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// True when the build has this encoder at all. Cached: one -encoders call.
static bool encoderInBuild(const char* encoder)
{
    static std::string listing;
    static bool asked = false;
    if (!asked)
    {
        asked = true;
        runFfmpeg("-hide_banner -encoders", listing, 8000);
    }
    if (listing.empty()) return true;   // no ffmpeg to ask; do not hide anything

    // The encoder field may carry fixed arguments ("ffv1 -level 3"), and the
    // listing pads names with spaces, so match the bare name with a boundary.
    std::string name(encoder);
    const size_t sp = name.find(' ');
    if (sp != std::string::npos) name = name.substr(0, sp);
    const std::string needle = " " + name + " ";
    return listing.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
//  Settings <-> controls
// ---------------------------------------------------------------------------
static void settingsToUi()
{
    g_loading = true;
    for (int i = 0; i < kSettingCount; ++i)
    {
        const Setting& s = kSettings[i];
        const std::string v = iniGet(s.key);
        switch (s.type)
        {
        case S_BOOL:
            SendMessageA(g_ctl[i], BM_SETCHECK, (v == "1") ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case S_CHOICE:
        {
            int idx = 0;
            if (s.choices && strchr(s.choices, '|') && !isdigit((unsigned char)(v.empty()?'x':v[0])))
            {
                // Textual choice, e.g. RenderMode=Video
                const int n = (int)SendMessageA(g_ctl[i], CB_GETCOUNT, 0, 0);
                for (int k = 0; k < n; ++k)
                {
                    char b[64]{}; SendMessageA(g_ctl[i], CB_GETLBTEXT, k, (LPARAM)b);
                    if (_stricmp(b, v.c_str()) == 0) { idx = k; break; }
                }
            }
            else idx = atoi(v.c_str());
            SendMessageA(g_ctl[i], CB_SETCURSEL, idx, 0);
            break;
        }
        case S_PRESET:
        {
            fillPresetCombo(g_ctl[i]);
            int idx = 0;
            const int n = (int)SendMessageA(g_ctl[i], CB_GETCOUNT, 0, 0);
            for (int k = 1; k < n; ++k)
            {
                char b[128]{}; SendMessageA(g_ctl[i], CB_GETLBTEXT, k, (LPARAM)b);
                if (_stricmp(b, v.c_str()) == 0) { idx = k; break; }
            }
            SendMessageA(g_ctl[i], CB_SETCURSEL, idx, 0);
            break;
        }
        default:
            SetWindowTextA(g_ctl[i], v.c_str());
            break;
        }
    }
    g_loading = false;
    refreshEnabled();
    setDirty(false);
}

// Hold every numeric to its stated range before it reaches the file, and say
// which ones moved.
//
// Not cosmetic. The mod raises SettleSubFrames=0 to 1 on load, so a zero in the
// file is a value the user typed, never sees corrected, and keeps re-reading as
// the reason their motion blur "does nothing" - when the truth is that it was
// never zero by the time the renderer ran. Same shape for Shutter, where the
// mod's own clamp is 0.01..2.0 and the label invites typing 180.
static std::string clampNumerics()
{
    std::string moved;
    char buf[128];
    for (int i = 0; i < kSettingCount; ++i)
    {
        const Setting& s = kSettings[i];
        if (s.type != S_INT && s.type != S_FLOAT) continue;
        if (s.lo == s.hi) continue;

        const std::string raw = editText(g_ctl[i]);
        if (raw.empty()) continue;

        double v = atof(raw.c_str());
        const double was = v;
        if (v < s.lo) v = s.lo;
        if (v > s.hi) v = s.hi;
        if (v == was) continue;

        if (s.type == S_INT) sprintf_s(buf, "%d", (int)(v + 0.5));
        else                 sprintf_s(buf, "%g", v);
        SetWindowTextA(g_ctl[i], buf);

        char line[256];
        sprintf_s(line, "%s  %s -> %s\r\n", s.label, raw.c_str(), buf);
        moved += line;
    }
    return moved;
}

static void uiToSettings()
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        const Setting& s = kSettings[i];
        switch (s.type)
        {
        case S_BOOL:
            iniSet(s.key, SendMessageA(g_ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            break;
        case S_CHOICE:
        {
            const int idx = (int)SendMessageA(g_ctl[i], CB_GETCURSEL, 0, 0);
            if (s.wordValue) iniSet(s.key, comboText(g_ctl[i]));
            else { char b[16]; sprintf_s(b, "%d", idx < 0 ? 0 : idx); iniSet(s.key, b); }
            break;
        }
        case S_PRESET:
        {
            const int idx = (int)SendMessageA(g_ctl[i], CB_GETCURSEL, 0, 0);
            iniSet(s.key, idx <= 0 ? "" : comboText(g_ctl[i]));
            break;
        }
        default:
            iniSet(s.key, editText(g_ctl[i]));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  Preset tab
// ---------------------------------------------------------------------------
// The combo can be missing entries the build does not have, so its index is
// NOT the table index - each item carries the real one.
static int selCodec()
{
    const int sel = (int)SendMessageA(g_pcodec, CB_GETCURSEL, 0, 0);
    if (sel < 0) return 0;
    const LRESULT i = SendMessageA(g_pcodec, CB_GETITEMDATA, sel, 0);
    return (i < 0 || i >= kCodecCount) ? 0 : (int)i;
}

static int optCount(const Opt* list)
{
    int n = 0;
    if (list) while (list[n].label) ++n;
    return n;
}

// Whatever the given dropdown currently selects out of `list`, or "".
static std::string optArgs(HWND combo, const Opt* list)
{
    const int n = optCount(list);
    if (!n) return "";
    int i = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= n) i = 0;
    return list[i].args ? list[i].args : "";
}

static void append(std::string& s, const std::string& part)
{
    if (part.empty()) return;
    if (!s.empty()) s += " ";
    s += part;
}

static std::string buildArgs()
{
    const Codec& c = kCodecs[selCodec()];

    std::string s = "-c:v ";
    s += c.encoder;

    append(s, optArgs(g_prate,  c.rate));
    append(s, optArgs(g_pspeed, c.speed));
    append(s, optArgs(g_ptune,  c.tune));
    append(s, optArgs(g_ppix,   c.pix));

    // Colour tagging, and the conversion flags that go with it.
    //
    // The renderer hands over RGB frames, so anything YUV is a conversion, and
    // an untagged file leaves the player to guess which primaries it was
    // converted with. Players guess 601 often enough that every hue shifts
    // slightly - the sort of error that survives a whole grade before anyone
    // notices. The two sws flags stop the conversion itself rounding badly.
    if (c.colour && SendMessageA(g_pcolour, BM_GETCHECK, 0, 0) == BST_CHECKED)
        append(s, "-sws_flags +accurate_rnd+full_chroma_int"
                  " -colorspace bt709 -color_primaries bt709"
                  " -color_trc bt709 -color_range tv");

    append(s, editText(g_pextra));
    return s;
}


static void previewPreset()
{
    const std::string s = "[Preset]\r\nArgs=" + buildArgs() + "\r\nExt=" + comboText(g_pext);
    SetWindowTextA(g_pargs, s.c_str());
}

static void fillOpts(HWND combo, const Opt* list)
{
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    const int n = optCount(list);
    for (int i = 0; i < n; ++i)
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)list[i].label);
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
    // A list of one is a statement, not a choice.
    EnableWindow(combo, n > 1);
}

// Test-encode one frame of the exact combination on screen.
static void testSelected(bool quiet)
{
    if (g_ffmpegPath.empty())
    {
        SetWindowTextA(g_pstatus, "ffmpeg not found - cannot check this preset.");
        return;
    }

    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    const std::string out = std::string(tmp) + "replus_preset_probe." + comboText(g_pext);
    DeleteFileA(out.c_str());

    const std::string cmd =
        "-hide_banner -loglevel error -y -f lavfi "
        "-i testsrc2=size=320x240:rate=25:duration=0.08 " + buildArgs() +
        " \"" + out + "\"";

    SetWindowTextA(g_pstatus, "Checking...");
    UpdateWindow(g_pstatus);

    std::string err;
    const bool ok = runFfmpeg(cmd, err, 30000);
    DeleteFileA(out.c_str());

    if (ok)
    {
        SetWindowTextA(g_pstatus, "OK - this combination encodes on this machine.");
        return;
    }

    // ffmpeg's last stderr line is the one that names the reason; everything
    // above it is usually the same failure restated by the muxer.
    std::string last;
    size_t e = err.find_last_not_of("\r\n");
    if (e != std::string::npos)
    {
        const size_t b = err.find_last_of('\n', e);
        last = err.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b));
    }
    if (last.empty()) last = "ffmpeg refused this combination.";
    if (last.size() > 150) last = last.substr(0, 150) + "...";
    SetWindowTextA(g_pstatus, ("FAILED - " + last).c_str());
    if (!quiet)
        MessageBoxA(nullptr, err.empty() ? "ffmpeg produced no output." : err.c_str(),
                    "RE+ Render Settings - preset check", MB_ICONWARNING);
}

static void codecChanged()
{
    const Codec& c = kCodecs[selCodec()];

    fillOpts(g_prate,  c.rate);
    fillOpts(g_pspeed, c.speed);
    fillOpts(g_ptune,  c.tune);
    fillOpts(g_ppix,   c.pix);

    SendMessageA(g_pext, CB_RESETCONTENT, 0, 0);
    for (int i = 0; c.exts[i]; ++i)
        SendMessageA(g_pext, CB_ADDSTRING, 0, (LPARAM)c.exts[i]);
    SendMessageA(g_pext, CB_SETCURSEL, 0, 0);

    EnableWindow(g_pcolour, c.colour);
    SendMessageA(g_pcolour, BM_SETCHECK, c.colour ? BST_CHECKED : BST_UNCHECKED, 0);

    SetWindowTextA(g_pnote, c.note);
    SetWindowTextA(g_pstatus, "");
}


static void refreshPresetList()
{
    SendMessageA(g_plist, LB_RESETCONTENT, 0, 0);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((g_presetDir + "*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        const size_t d = n.rfind('.');
        if (d != std::string::npos) n = n.substr(0, d);
        SendMessageA(g_plist, LB_ADDSTRING, 0, (LPARAM)n.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void savePreset()
{
    std::string name = editText(g_pname);
    if (name.empty())
    {
        MessageBoxA(nullptr, "Give the preset a name first.\n\nThat name is what goes in "
            "Render.ini as the encoder preset.", "RE+ Render Settings", MB_ICONINFORMATION);
        return;
    }
    for (char& c : name)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';

    CreateDirectoryA(g_presetDir.c_str(), nullptr);
    const Codec& c = kCodecs[selCodec()];
    const std::string text =
        "; " + std::string(c.note) + "\r\n"
        "; Use with RenderVideoPreset=" + name + " in Render.ini\r\n\r\n"
        "[Preset]\r\nArgs=" + buildArgs() + "\r\nExt=" + comboText(g_pext) + "\r\n";

    HANDLE h = CreateFileA((g_presetDir + name + ".ini").c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        char m[512];
        sprintf_s(m, "Could not write into:\n%s\n\nError %lu. Nothing here needs "
            "administrator rights - if the game is under Program Files, the mod keeps "
            "its files in LocalAppData instead.", g_presetDir.c_str(), GetLastError());
        MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONERROR);
        return;
    }
    DWORD w = 0; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr); CloseHandle(h);

    refreshPresetList();
    for (int i = 0; i < kSettingCount; ++i)
        if (kSettings[i].type == S_PRESET) fillPresetCombo(g_ctl[i]);

    char m[256];
    sprintf_s(m, "Saved %s.ini\n\nPick it under Encoder preset on the Render tab to use it.",
              name.c_str());
    MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONINFORMATION);
}

static void loadPreset()
{
    const int sel = (int)SendMessageA(g_plist, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    char n[128]{}; SendMessageA(g_plist, LB_GETTEXT, sel, (LPARAM)n);
    const std::string file = g_presetDir + n + ".ini";

    char args[1024]{}, ext[64]{};
    GetPrivateProfileStringA("Preset", "Args", "", args, sizeof(args), file.c_str());
    GetPrivateProfileStringA("Preset", "Ext",  "", ext,  sizeof(ext),  file.c_str());

    SetWindowTextA(g_pname, n);
    for (int i = 0; i < kCodecCount; ++i)
        if (strstr(args, kCodecs[i].encoder))
        { SendMessageA(g_pcodec, CB_SETCURSEL, i, 0); codecChanged(); break; }

    const int n2 = (int)SendMessageA(g_pext, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n2; ++i)
    { char b[32]{}; SendMessageA(g_pext, CB_GETLBTEXT, i, (LPARAM)b);
      if (_stricmp(b, ext) == 0) { SendMessageA(g_pext, CB_SETCURSEL, i, 0); break; } }

    const std::string s = "[Preset]\r\nArgs=" + std::string(args) + "\r\nExt=" + ext;
    SetWindowTextA(g_pargs, s.c_str());
}

// ---------------------------------------------------------------------------
//  Tabs
// ---------------------------------------------------------------------------
static void showTab(int t)
{
    for (int i = 0; i < kSettingCount; ++i)
    {
        ShowWindow(g_ctl[i],      t == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(g_ctlLabel[i], t == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int g = 0; g < G_COUNT; ++g) ShowWindow(g_box[g], t == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_presetTabCtls) ShowWindow(h, t == 1 ? SW_SHOW : SW_HIDE);
}

// ---------------------------------------------------------------------------
//  Window
// ---------------------------------------------------------------------------
static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
               HWND p, int id)
{
    return CreateWindowA(cls, text, WS_CHILD | style, x, y, w, h, p, (HMENU)(INT_PTR)id,
                         nullptr, nullptr);
}

// Layout constants, in one place so a row added to the table cannot silently
// overlap the panel below it - which is what happened every time these were
// spelled out at the point of use.
enum {
    LAY_W      = 764,   // client width
    LAY_COLW   = 366,   // a column group box
    LAY_COLX0  = 12,
    LAY_COLX1  = 386,
    LAY_FULLW  = 740,   // a full-width group box
    LAY_TOP    = 40,    // first group starts below the tab strip
    LAY_ROW    = 26,
    LAY_BOXTOP = 20,    // inside a group box, before the first row
    LAY_BOXBOT = 10,
    LAY_GAP    = 8,
    LAY_LABELW = 156,
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_main = hwnd;
        g_tip = CreateWindowExA(0, TOOLTIPS_CLASSA, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, 460);
        SendMessageA(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

        g_tab = CreateWindowA(WC_TABCONTROLA, nullptr, WS_CHILD | WS_VISIBLE,
            8, 8, LAY_W - 16, 24, hwnd, (HMENU)ID_TAB, nullptr, nullptr);
        TCITEMA ti{}; ti.mask = TCIF_TEXT;
        ti.pszText = (LPSTR)"Render";          SendMessageA(g_tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
        ti.pszText = (LPSTR)"Encoder presets"; SendMessageA(g_tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);

        // --- Render tab: group boxes down two columns, then full width -------
        //
        // Everything is measured from the table, so adding a setting grows its
        // box and pushes what follows down. The previous layout divided the
        // rows by count and then placed the wide rows at a computed offset,
        // which meant a new entry could land on top of the help panel.
        int colY[3] = { LAY_TOP, LAY_TOP, 0 };

        for (int g = 0; g < G_COUNT; ++g)
        {
            int rows = 0;
            for (int i = 0; i < kSettingCount; ++i) if (kSettings[i].group == g) ++rows;
            if (!rows) continue;

            const int col = kGroups[g].col;
            if (col == 2 && colY[2] == 0)
                colY[2] = (colY[0] > colY[1] ? colY[0] : colY[1]) + LAY_GAP;

            const int bx = (col == 1) ? LAY_COLX1 : LAY_COLX0;
            const int bw = (col == 2) ? LAY_FULLW : LAY_COLW;
            const int bh = LAY_BOXTOP + rows * LAY_ROW + LAY_BOXBOT;
            const int by = colY[col];

            g_box[g] = mk("BUTTON", kGroups[g].title, WS_VISIBLE | BS_GROUPBOX,
                          bx, by, bw, bh, hwnd, 0);

            int ry = by + LAY_BOXTOP;
            for (int i = 0; i < kSettingCount; ++i)
            {
                const Setting& s = kSettings[i];
                if (s.group != g) continue;

                const int lx = bx + 12;
                const int cx = lx + LAY_LABELW;
                g_ctlLabel[i] = mk("STATIC", s.label, WS_VISIBLE, lx, ry + 4,
                                   LAY_LABELW - 6, 18, hwnd, 0);

                if (s.type == S_BOOL)
                    g_ctl[i] = mk("BUTTON", "", WS_VISIBLE | BS_AUTOCHECKBOX,
                                  cx, ry + 3, 20, 18, hwnd, ID_SETTING + i);
                else if (s.type == S_CHOICE || s.type == S_PRESET)
                {
                    const int w = (col == 2) ? 260 : 170;
                    g_ctl[i] = mk("COMBOBOX", "", WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  cx, ry, w, 240, hwnd, ID_SETTING + i);
                    if (s.choices)
                    {
                        std::string all = s.choices, cur;
                        for (char ch : all + "|")
                        { if (ch == '|') { if (!cur.empty()) SendMessageA(g_ctl[i], CB_ADDSTRING, 0, (LPARAM)cur.c_str()); cur.clear(); }
                          else cur += ch; }
                    }
                }
                else if (s.type == S_TEXT)
                {
                    g_ctl[i] = mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  cx, ry, bx + bw - 12 - cx, 22, hwnd, ID_SETTING + i);
                    setCue(g_ctl[i], s.key);
                }
                else
                    g_ctl[i] = mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  cx, ry, 90, 22, hwnd, ID_SETTING + i);

                addTip(g_ctl[i], s.help);
                addTip(g_ctlLabel[i], s.help);
                ry += LAY_ROW;
            }
            colY[col] = by + bh + LAY_GAP;
        }

        const int renderBottom = colY[2] > 0 ? colY[2]
                                             : (colY[0] > colY[1] ? colY[0] : colY[1]);

        // --- Preset tab -----------------------------------------------------
        //
        // Sized from where the RENDER tab ended, not from its own contents.
        // The two tabs share one window, so the shorter one decides nothing:
        // laid out to its own height it left a third of the window empty below
        // the preview box, with the help panel stranded far underneath.
        auto keep = [&](HWND h) { g_presetTabCtls.push_back(h); return h; };

        const int tabH  = renderBottom - LAY_TOP - LAY_GAP;
        const int lstW  = 172;
        const int bldX  = LAY_COLX0 + lstW + LAY_GAP;
        const int bldW  = LAY_FULLW - lstW - LAY_GAP;

        keep(mk("BUTTON", "Saved presets", WS_VISIBLE | BS_GROUPBOX,
                LAY_COLX0, LAY_TOP, lstW, tabH, hwnd, 0));

        const int btnsH = 2 * 26 + 4 + 8;                     // Save + Delete
        const int lstH  = tabH - LAY_BOXTOP - btnsH - LAY_BOXBOT;
        g_plist = keep(mk("LISTBOX", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                          LAY_COLX0 + 12, LAY_TOP + LAY_BOXTOP, lstW - 24, lstH, hwnd, ID_PLIST));
        const int btnY = LAY_TOP + LAY_BOXTOP + lstH + 8;
        keep(mk("BUTTON", "Save preset", WS_VISIBLE | BS_PUSHBUTTON,
                LAY_COLX0 + 12, btnY, lstW - 24, 26, hwnd, ID_PSAVE));
        keep(mk("BUTTON", "Delete", WS_VISIBLE | BS_PUSHBUTTON,
                LAY_COLX0 + 12, btnY + 30, lstW - 24, 26, hwnd, ID_PDEL));

        keep(mk("BUTTON", "Build a preset", WS_VISIBLE | BS_GROUPBOX,
                bldX, LAY_TOP, bldW, tabH, hwnd, 0));

        // Two text rows bracketing six dropdowns. The order is the order the
        // arguments come out in, which makes the preview box below readable as
        // a left-to-right consequence of the rows above it.
        struct Row { const char* l; int id; bool edit; };
        static const Row rows[] = {
            { "Name",             ID_PNAME,  true  },
            { "Codec",            ID_PCODEC, false },
            { "Quality / rate",   ID_PRATE,  false },
            { "Encoding speed",   ID_PSPEED, false },
            { "Tune",             ID_PTUNE,  false },
            { "Pixel format",     ID_PPIX,   false },
            { "Container",        ID_PEXT,   false },
            { "Extra arguments",  ID_PEXTRA, true  },
        };
        const int rlx = bldX + 12;
        const int rcx = rlx + 110;
        const int rcw = bldW - 24 - 110;
        int py = LAY_TOP + LAY_BOXTOP;
        for (const Row& r : rows)
        {
            keep(mk("STATIC", r.l, WS_VISIBLE, rlx, py + 4, 104, 18, hwnd, 0));
            HWND h = r.edit
                ? mk("EDIT", r.id == ID_PNAME ? "my_preset" : "",
                     WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rcx, py, rcw, 22, hwnd, r.id)
                : mk("COMBOBOX", "", WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     rcx, py, rcw, 320, hwnd, r.id);
            keep(h);
            switch (r.id)
            {
            case ID_PNAME:  g_pname  = h; break;
            case ID_PCODEC: g_pcodec = h; break;
            case ID_PRATE:  g_prate  = h; break;
            case ID_PSPEED: g_pspeed = h; break;
            case ID_PTUNE:  g_ptune  = h; break;
            case ID_PPIX:   g_ppix   = h; break;
            case ID_PEXT:   g_pext   = h; break;
            case ID_PEXTRA: g_pextra = h; break;
            }
            py += 30;
        }

        g_pcolour = keep(mk("BUTTON", "Tag colour as BT.709",
                            WS_VISIBLE | BS_AUTOCHECKBOX, rcx, py + 2, 190, 20, hwnd, ID_PCOLOUR));
        keep(mk("BUTTON", "Test preset", WS_VISIBLE | BS_PUSHBUTTON,
                rcx + 200, py, 110, 24, hwnd, ID_PTEST));
        py += 28;

        // Says whether the combination on screen actually encodes here. The
        // codec list already hides what the build lacks; this catches what the
        // build has and the MACHINE lacks, which is every QSV and AMF entry on
        // a machine without that hardware.
        g_pstatus = keep(mk("STATIC", "", WS_VISIBLE, rlx, py + 4, bldW - 24, 18, hwnd, ID_PSTATUS));
        py += 24;

        // A read-only multiline EDIT, not a STATIC - the same reason the help
        // panel below gives, which this control predated and did not follow.
        // A static clips silently and no height stays safe: ProRes alone is
        // three paragraphs and lost its last two lines. Scrolling degrades;
        // clipping just hides the sentence that explains the setting.
        //
        // Both boxes now take whatever height is left rather than a constant,
        // so the codec note is read without scrolling on every codec but one.
        const int boxW    = bldW - 24;
        const int spare   = (LAY_TOP + tabH - LAY_BOXBOT) - (py + 8) - 8;
        const int noteH   = spare * 6 / 10;
        const int argsH   = spare - noteH;
        keep(mk("STATIC", "About this codec", WS_VISIBLE, rlx, py + 6, 200, 16, hwnd, 0));
        g_pnote = keep(mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                          ES_READONLY | WS_VSCROLL, rlx, py + 24, boxW, noteH - 24,
                          hwnd, ID_PNOTE));
        keep(mk("STATIC", "What gets written", WS_VISIBLE, rlx, py + noteH + 10, 200, 16, hwnd, 0));
        g_pargs = keep(mk("EDIT", "", WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                          ES_READONLY | WS_VSCROLL, rlx, py + noteH + 28, boxW, argsH - 28,
                          hwnd, ID_PARGS));

        // --- Shared bottom ---------------------------------------------------
        //
        // Positioned from where the render tab ended. Hardcoding these put the
        // help and path labels straight through the last two wide rows.
        int bottom = renderBottom + 6;

        // A read-only multiline EDIT, not a STATIC.
        //
        // A static clips whatever does not fit and there is no height that is
        // safe: it was one line, then two, and RenderMode alone needs four once
        // its first line wraps. An edit scrolls instead, so the panel cannot
        // clip no matter how long a future help string gets - the failure mode
        // becomes "scroll a little", not "the last sentence is invisible".
        g_help = CreateWindowA("EDIT",
            "Hover any setting, or click into it, for an explanation. "
            "A greyed-out setting is one the renderer will not read with the options you have chosen.",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            12, bottom, LAY_FULLW, 92, hwnd, (HMENU)ID_HELP, nullptr, nullptr);
        bottom += 98;

        g_pathLabel = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
            12, bottom, LAY_FULLW, 18, hwnd, (HMENU)ID_PATH, nullptr, nullptr);
        bottom += 22;

        CreateWindowA("BUTTON", "Save Render.ini", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            LAY_FULLW - 118, bottom, 130, 28, hwnd, (HMENU)ID_SAVE, nullptr, nullptr);
        CreateWindowA("BUTTON", "Reload", WS_CHILD | WS_VISIBLE,
            LAY_FULLW - 206, bottom, 80, 28, hwnd, (HMENU)ID_RELOAD, nullptr, nullptr);
        bottom += 28 + 12;

        // Fit the window to the content rather than guessing a height.
        RECT want{ 0, 0, LAY_W, bottom };
        AdjustWindowRect(&want, (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE), FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(hwnd, [](HWND c, LPARAM p) -> BOOL
            { SendMessageA(c, WM_SETFONT, (WPARAM)p, TRUE); return TRUE; }, (LPARAM)f);

        resolvePaths();
        g_ffmpegPath = findFfmpeg();

        // Group prefix in the label, rather than two dropdowns. Twenty-four
        // entries is a long list to scan and the prefix is what makes it
        // scannable; a second combo would be a click in the way of every
        // selection to save one line of window.
        for (int i = 0; i < kCodecCount; ++i)
        {
            if (!encoderInBuild(kCodecs[i].encoder)) continue;
            std::string label = std::string(kCodecs[i].group) + " - " + kCodecs[i].name;
            const int at = (int)SendMessageA(g_pcodec, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            SendMessageA(g_pcodec, CB_SETITEMDATA, at, (LPARAM)i);
        }
        SendMessageA(g_pcodec, CB_SETCURSEL, 0, 0);
        codecChanged(); previewPreset();

        loadIni();
        settingsToUi();
        refreshPresetList();
        SetWindowTextA(g_pathLabel, ("Editing: " + g_iniPath).c_str());
        showTab(0);
        return 0;
    }

    case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == ID_TAB && ((LPNMHDR)lp)->code == TCN_SELCHANGE)
            showTab((int)SendMessageA(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id >= ID_SETTING && id < ID_SETTING + kSettingCount)
        {
            const int i    = id - ID_SETTING;
            const int code = HIWORD(wp);

            if (code == BN_CLICKED || code == CBN_SELCHANGE || code == EN_SETFOCUS)
                SetWindowTextA(g_help, kSettings[i].help);

            if (code == BN_CLICKED || code == CBN_SELCHANGE || code == EN_CHANGE)
            {
                if (!g_loading) setDirty(true);
                // Any change can move something else in or out of relevance -
                // Samples decides Shutter, JPEG decides its quality, the preset
                // decides the two argument boxes - so this runs for all of them
                // rather than for a list that would need extending each time.
                refreshEnabled();
            }
            return 0;
        }
        switch (id)
        {
        case ID_PCODEC:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                codecChanged();
                previewPreset();
                // Only on a codec change: it is the one that can be flatly
                // unavailable, and it is cheap. Everything else waits for the
                // button rather than putting a subprocess behind every click.
                testSelected(true);
            }
            return 0;
        case ID_PRATE: case ID_PEXT: case ID_PPIX: case ID_PSPEED: case ID_PTUNE:
            if (HIWORD(wp) == CBN_SELCHANGE) { previewPreset(); SetWindowTextA(g_pstatus, ""); }
            return 0;
        case ID_PCOLOUR:
            if (HIWORD(wp) == BN_CLICKED) { previewPreset(); SetWindowTextA(g_pstatus, ""); }
            return 0;
        case ID_PEXTRA:
            if (HIWORD(wp) == EN_CHANGE) { previewPreset(); SetWindowTextA(g_pstatus, ""); }
            return 0;
        case ID_PTEST: testSelected(false); return 0;
        case ID_PLIST:  if (HIWORD(wp) == LBN_SELCHANGE) loadPreset(); return 0;
        case ID_PSAVE:  savePreset(); return 0;
        case ID_PDEL:
        {
            const int sel = (int)SendMessageA(g_plist, LB_GETCURSEL, 0, 0);
            if (sel < 0) return 0;
            char n[128]{}; SendMessageA(g_plist, LB_GETTEXT, sel, (LPARAM)n);
            char q[256]; sprintf_s(q, "Delete %s.ini?\n\nThe renderer writes the shipped "
                                      "presets back if they go missing.", n);
            if (MessageBoxA(nullptr, q, "RE+ Render Settings", MB_YESNO | MB_ICONQUESTION) == IDYES)
            { DeleteFileA((g_presetDir + n + ".ini").c_str()); refreshPresetList(); }
            return 0;
        }
        case ID_SAVE:
        {
            const std::string moved = clampNumerics();
            uiToSettings();
            if (saveIni())
            {
                setDirty(false);
                if (moved.empty())
                    SetWindowTextA(g_help, "Saved. Comments in the file were left as they were.");
                else
                    SetWindowTextA(g_help,
                        ("Saved, with these held to their accepted range:\r\n\r\n" + moved +
                         "\r\nThe renderer would have done the same on load, silently.").c_str());
            }
            else
            {
                char m[512];
                sprintf_s(m, "Could not write:\n%s\n\nError %lu.", g_iniPath.c_str(), GetLastError());
                MessageBoxA(nullptr, m, "RE+ Render Settings", MB_ICONERROR);
            }
            return 0;
        }
        case ID_RELOAD:
            loadIni(); settingsToUi();
            SetWindowTextA(g_help, "Reloaded from disk.");
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_dirty &&
            MessageBoxA(hwnd, "There are unsaved changes.\n\nClose without saving?",
                        "RE+ Render Settings", MB_YESNO | MB_ICONQUESTION) == IDNO)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSA wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "REPlusRenderSettings";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    HWND w = CreateWindowA(wc.lpszClassName, "Rockstar Editor+ - Render Settings",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, LAY_W + 16, 600, nullptr, nullptr, hInst, nullptr);
    if (!w) return 1;

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageA(w, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
