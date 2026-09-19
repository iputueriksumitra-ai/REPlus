cmake_minimum_required(VERSION 3.15)

project(RockstarEditorPlus)

# STATIC CRT. Not a preference - a correctness requirement for a .asi.
#
# An ASI is injected into someone else's process and does not get to choose which
# msvcp140.dll is already loaded there. MSVC's STL requires the runtime DLL to be
# AT LEAST AS NEW as the toolset that compiled the headers, and a host is free to
# ship an older one: FiveM carries msvcp140 14.34 in FiveM.app\bin, which wins
# over the system's 14.51, so a build made with the current toolset binds its
# inlined 14.51 STL to a 2022-era runtime. That combination crashed on the first
# std::mutex lock the mod performed - inside MSVCP140!_Mtx_lock, reproducibly,
# and immune to every timing and threading change tried, because it is an ABI
# mismatch rather than a race.
#
# Linking the CRT statically ends the whole class of problem: the mod carries its
# own STL and no longer cares what the host loaded. It also drops the VC++
# redistributable from the install requirements, which is worth having on its
# own for singleplayer users.
#
# Everything linked in must agree, hence CMAKE_MSVC_RUNTIME_LIBRARY set before
# any target is declared (minhook included) rather than per-target.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB_RECURSE PROJECT_SOURCE_FILES "src/*.h" "src/*.cpp")
# Exclude the vendored minhook tree from the ASI target (it builds separately).
list(FILTER PROJECT_SOURCE_FILES EXCLUDE REGEX "src/vendor/")

macro(GroupSources curdir groupindex)
    file(GLOB children RELATIVE ${curdir} ${curdir}/*)
    foreach(child ${children})
        if(IS_DIRECTORY ${curdir}/${child})
            GroupSources(${curdir}/${child} ${groupindex}/${child})
        else()
            string(REPLACE "/" "\\" groupname ${groupindex})
            source_group(${groupname} FILES ${curdir}/${child})
        endif()
    endforeach()
endmacro()

GroupSources(${CMAKE_SOURCE_DIR}/src "Source Files")

add_subdirectory("src/vendor/minhook")

add_library(RockstarEditorPlus SHARED ${PROJECT_SOURCE_FILES} fx_asi_build.rc)
add_dependencies(RockstarEditorPlus minhook)

include_directories(
    ./src
)

set_target_properties(RockstarEditorPlus PROPERTIES
    OUTPUT_NAME "RockstarEditorPlus"
    SUFFIX ".asi"
    CXX_STANDARD 20
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)

# =============================================================================
#  The shipped .ini files
# =============================================================================
#  GENERATED, not tracked. The .ini is a build output like the .asi: it belongs
#  beside the binary, which is where the mod reads it and where the in-editor
#  menu writes toggles back to. Keeping a copy in the repo as well meant two
#  files that drift, and a working tree that collects edited settings from
#  whatever was last tested.
#
#  So the authoritative text lives HERE, is written into the build tree at
#  configure time, and is copied next to the .asi on every build. Nothing
#  ini-shaped exists in the source tree at all.
#
#  Editing note: these are bracket arguments, so everything inside is literal -
#  no escaping needed for the ";" comment markers or for backslashes in paths.
# =============================================================================
set(REP_INI_MAIN [==[
; RockstarEditorPlus - editor settings.   Render settings are in Render.ini.
; Every row is also in the in-game menu, which explains each one and writes here.
; Inline "; ..." is safe after a NUMBER. Never after a path or a word.

[RockstarEditorPlus]

; --- Camera -----------------------------------------------------------------
Enabled=1                  ; 0 = stock game, no hooks at all
SplinePosition=1           ; curved path instead of the straight-line blend
SplineOrientation=1        ; splined rotation; look-at markers re-aim instead
SplineFov=1                ; monotone zoom curve, no overshoot
SplineAttached=1           ; also spline shots mounted on a car/bike/ped
LookAtReaim=1              ; re-aim look-at markers after moving the camera
LookAtAspect=0             ; aspect for that re-aim; 0 = the display's
OnlySmoothBlend=1          ; 1 = only "Smooth" markers, 0 = "Linear" too

; Pacing. Natural is the default and overrides the two below it.
NaturalPacing=1            ; path driven by marker times - eases into bends
SmoothSpeedProfile=1       ; one speed curve across markers
ArcLengthRemap=1           ; per-segment fallback, used only when both are 0
Alpha=1                    ; tension: 0 uniform / 0.5 centripetal / 1 chordal
                           ; no effect while NaturalPacing=1
SplineWeight=0             ; camera mass on playback: 0 hits markers, 1 carried past

; --- Shake ------------------------------------------------------------------
; Defaults each marker inherits; per-marker, set its Shake row to "Rockstar
; Editor+". Handheld starting point: 2.0 @ 0.20.
ShakeSimpleMode=1          ; 1 = Amplitude/Frequency, 0 = the Sway/Jitter model
ShakeAmplitude=1.0         ; degrees; 0.05 m of travel per degree
ShakeFrequency=0.35        ; Hz
ShakeVariation=0.7         ; 0 = metronomic, higher = less regular
ShakeRoughness=3
ShakeSeed=0                ; reroll without changing the character

; Motion coupling - shake harder when the camera is moving.
ShakeSpeedAmp=0.0          ; extra amplitude at reference speed. Try 1.0
ShakeSpeedFreq=0.0         ; extra rate at reference speed. Try 0.5
ShakeSpeedRef=30.0         ; m/s that counts as "fully moving"
ShakeStopWhenStill=0       ; 1 = fade out when parked

; Complex mode only - ignored while ShakeSimpleMode=1.
SwayMove=0.0
SwayRotate=0.0
SwayFrequency=1.8
JitterMove=0.0
JitterRotate=0.0
JitterFrequency=9.0

; Per-axis weights. 0 disables an axis, e.g. Roll=0 for a locked horizon.
ShakeAxisLateral=1.0
ShakeAxisForward=0.6
ShakeAxisVertical=1.0
ShakeAxisPitch=1.0
ShakeAxisRoll=1.0
ShakeAxisYaw=1.0

; --- Editor limits ----------------------------------------------------------
UnlimitedCameraDistance=1  ; lift the 30m leash off the player
MaxCameraDistance=20000    ; metres
StreamingFocusOnCamera=1   ; stream map, collision and peds around the CAMERA
DisableCameraCollision=1   ; camera passes through geometry
BypassProfanityFilter=1    ; naming/export still works with Social Club offline
UnlockCameraRestrictions=1 ; free camera on first-person clips
UncapZoom=1                ; widen the stock 0.45x..4.50x range
ZoomMinFov=1               ; 1..130 is the engine's own clamp
ZoomMaxFov=130             ; readout is 45/fov: 1 = 45x in, 130 = 0.35x out

; --- The stall between every action ("buffering") ---------------------------
FastPrecache=1             ; skip the wait for global streaming idle. 0 = stock
FastPrecacheAudio=1        ; and its separate audio wait
FastPrecacheDuringRender=0 ; renders keep the stock wait; 1 only to A/B it
PrecacheMaxMs=1500         ; ceiling on the preload wait. 0 = stock (unbounded)
UrgentModelLoadMs=250      ; ceiling on a blocking model load. 0 = stock (5000)

; --- Scene: the clip's time of day and weather ------------------------------
; Substituted as the frame plays. Nothing is written to the .clip.
OverrideTimeOfDay=0        ; 1 = use TimeOfDay instead of the recorded clock
TimeOfDay=720              ; minutes past midnight. 720 = 12:00, 1065 = 17:45

OverrideWeather=0          ; 1 = use the four settings below
; 0 EXTRASUNNY 1 CLEAR 2 CLOUDS 3 SMOG 4 FOGGY 5 OVERCAST 6 RAIN 7 THUNDER
; 8 CLEARING 9 NEUTRAL 10 SNOW 11 BLIZZARD 12 SNOWLIGHT 13 XMAS 14 HALLOWEEN
WeatherType=0
WeatherBlendTo=-1          ; second type to blend towards, -1 = none
WeatherBlend=0.0           ; 0..1 between WeatherType and WeatherBlendTo
WeatherWetness=-1          ; 0..1 wet roads and puddles; -1 = as recorded
LiveTimecycle=1            ; master switch. 0 keeps the clip's baked lighting

; WOW TEAM SCENE - Cloud Hat playback overlay. Separate from Weather above.
OverrideCloudHat=0         ; 1 = force CloudHatType while Rockstar Editor is active
CloudHatType=4             ; index from the Scene Clouds page; 4 = Cloudy 01
CloudHatOpacity=1.0        ; 0..1

; --- Recording length -------------------------------------------------------
ReplayBlocks=128           ; 3..128. 128 = ~1m45 of city, ~604 MB. Restart first

; --- Diagnostics ------------------------------------------------------------
ShakeDebugLog=0
SplineDebugLog=0
SplineDebugGain=1          ; exaggerate the curve to see it. 0..100
SplineTraceLog=0           ; one line per frame - noisy

; Menu state, written for you when you change a row in-game.
MenuExpanded=0
MenuGroup=0
MenuStep=2
]==])

set(REP_INI_RENDER [==[
; RockstarEditorPlus - render settings.
; "RE+ Render Settings.exe" in this folder edits every key and explains the cost.
; Inline "; ..." is safe after a NUMBER. Never after a path or a word.

[Render]

; --- Capture ----------------------------------------------------------------
EnableRenderer=0           ; master switch. Off = the game's own Export runs

; Sliding = the clip plays; particles and TAA/SSR/RT stay live. Faster at 360.
; Walking = pause and seek per sample; exact shutter. Better below 0.5 shutter.
RenderCaptureMode=Sliding

; The lens, and independent of the mode above. Walking+DoF is a frozen instant
; through a real aperture; Sliding+DoF keeps the world running across the
; exposure. Costs the same either way, and both are FAR slower than no lens.
RenderDepthOfField=0
RenderFps=30               ; output rate, independent of your actual framerate
RenderSamples=64           ; renders averaged per frame. 1 = no motion blur
RenderShutter=1            ; 1.0 = 360 degrees, 0.5 = the 180 film convention
RenderSettleFrames=3       ; Walking: redraw wait after a new-frame seek
RenderSettleSubFrames=1    ; Walking: wait between sub-samples. Never 0
RenderHighlightBoost=0.9   ; keeps speculars bright through accumulation

; --- IGCS Depth of Field ----------------------------------------------------
; Bokeh SHAPE stays in ReShade's IGCS Connector panel - it is judged by eye.
RenderDofBokehSize=0.15    ; aperture width. Bigger = shallower focus
RenderDofQuality=12        ; rings of samples. Higher = smoother bokeh, slower
RenderDofAutofocus=1       ; measure focus in the world each frame
RenderJpeg=0               ; 0 = PNG
RenderQuality=100          ; JPEG only. Below 91 = chroma subsampled
RenderHideHud=1            ; hide the editor HUD and cursor while rendering
ExportCloseWhenDone=1      ; return to the editor menus when done
; Empty = RockstarEditorPlus\Captures\
RenderOutputFolder=
; --- Output -----------------------------------------------------------------
; Video = streamed to ffmpeg and deleted. Frames = sequence + assemble.txt.
RenderMode=Video
RenderKeepFrames=0         ; Video mode: keep the frames too
; Empty = the bundled copy, then beside the exe, then PATH.
FfmpegPath=

; Named preset from presets\ - see that folder, or the Encoder presets tab in
; RE+ Render Settings. Blank it to use the two keys below instead.
RenderVideoPreset=
; IGNORED while a preset is named above - it supplies both of these.
RenderVideoArgs=-c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p
RenderVideoExt=mp4

; --- Audio ------------------------------------------------------------------
RenderAudio=1              ; one Export: a sound pass, then the frames
RenderMarkerSpeed=1        ; render a marker's slow/fast motion. 0 = ignore it
; Borrow a track from a file instead. Ignored while RenderAudio=1.
AudioFromFile=
]==])

set(REP_INI_LIGHTS [==[
; RockstarEditorPlus - scene lights.
;
; The whole set is re-read every 2 seconds, so lights can be added, moved,
; retuned and toggled without restarting the game.
;
; This file is the DEFAULT set: what free roam uses, and what a clip you have
; never lit starts from. Per-clip lights live in lights\scenelights.txt and
; are saved with the project, not here.

[Lights]
; How many [LightN] sections below to read. Max 32.
;
; ZERO by default, deliberately: the light list is consumed on every frame the
; game renders, not just in the editor, so a light that ships enabled would be
; lighting the world before anyone asked for one. Add lights from the editor
; menu - Rockstar Editor+ > Scene Lights - or raise this and fill in the
; sections below.
Count=0

; ---------------------------------------------------------------------------
; Reference for the hand-edited form. Ignored while Count=0.
; ---------------------------------------------------------------------------
; [Light0]
; Enabled=1
; Type=0            ; 0 = point, 1 = spot
; X=0.0
; Y=0.0
; Z=0.0
; R=1.0
; G=0.85
; B=0.60
; Intensity=10.0
; Range=40.0
; Falloff=8.0
; CastShadows=1     ; only eight lights in a scene get a shadow map
;
; Spot only. Aim in degrees, same convention as the free camera: yaw 0 faces
; +Y, positive pitch tilts up, so -90 points straight down.
; Pitch=-90.0
; Yaw=0.0
; ConeInner=10.0
; ConeOuter=45.0
]==])

# Staged inside the build tree, never in the source tree.
file(WRITE "${CMAKE_BINARY_DIR}/ini/RockstarEditorPlus.ini" "${REP_INI_MAIN}")
file(WRITE "${CMAKE_BINARY_DIR}/ini/Render.ini"             "${REP_INI_RENDER}")
file(WRITE "${CMAKE_BINARY_DIR}/ini/Lights.ini"             "${REP_INI_LIGHTS}")

# copy_if_different, so a user's edited settings beside the .asi are not
# clobbered on every rebuild - only replaced when the shipped text really changed.
add_custom_command(TARGET RockstarEditorPlus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ini/RockstarEditorPlus.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/RockstarEditorPlus.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ini/Render.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/Render.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ini/Lights.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/Lights.ini"
    COMMENT "Writing the generated .ini files next to the .asi")

set_target_properties( minhook
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/LIB"
)

# smoothblend.cpp guards the detour body with __try/__except; MSVC needs SEH
# enabled and it must not be compiled with /EHsc-only semantics there.
target_compile_options(RockstarEditorPlus PRIVATE /utf-8)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
if(ENABLE_LOGS)
    target_compile_definitions(RockstarEditorPlus PRIVATE _LOG)
endif()

target_link_directories(RockstarEditorPlus PUBLIC "${CMAKE_BINARY_DIR}/LIB")
target_link_libraries(RockstarEditorPlus version.lib shell32.lib debug minhook.x64d.lib optimized minhook.x64.lib)

# =============================================================================
#  Preset Maker - a small GUI for authoring encoder presets
# =============================================================================
#  Separate executable, not part of the .asi. Encoder presets are two-line inis
#  that are easy to write and easy to get subtly wrong: a container that cannot
#  hold the chosen codec makes ffmpeg refuse, and the render then produces
#  nothing for a reason the user never sees. The tool makes that pairing
#  impossible to express.
#
#  Win32 with no dependencies and the same static CRT as the .asi, so it runs
#  from the bundle on a machine with nothing installed.
# =============================================================================
add_executable(PresetMaker WIN32 tools/presetmaker/presetmaker.cpp)
set_target_properties(PresetMaker PROPERTIES
    # The name it ships under, which is also what the tool calls itself in its
    # own title bars and message boxes. It stopped being a preset maker when it
    # grew a table of every Render.ini key; only this line still said otherwise,
    # so a build produced "RE+ Preset Maker.exe" next to the shipped
    # "RE+ Render Settings.exe" and left two of them in the folder.
    OUTPUT_NAME "RE+ Render Settings"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)
target_compile_options(PresetMaker PRIVATE /utf-8)
