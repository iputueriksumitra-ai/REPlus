# WOW TEAM SCENE - Rockstar Editor+ Cloud Hat Extension V2

V2 fixes the FiveM Rockstar Editor execution path.

## What changed

- Cloud Hat native calls no longer depend on ScriptHookV's scheduled fiber while Rockstar Editor owns playback.
- On FiveM, the extension prefers `scripting-gta.dll` and calls the exported `fx::ScriptEngine::CallNativeHandler` bridge directly.
- The existing ScriptHookV bridge remains as a fallback for non-FiveM/ordinary ASI environments.
- Cloud Hat updates run from RE+'s own editor/Scaleform heartbeat, which remains active during Rockstar Editor playback.
- New Cloud Hats use a short `PRELOAD_CLOUD_HAT` -> `LOAD_CLOUD_HAT` state transition before opacity is applied.
- `LOAD_CLOUD_HAT` is still state-driven and is NOT spammed every frame.

## Test

Rockstar Editor+ -> Scene Clouds

- Cloud Hat: Cloudy 01
- Cloud Opacity: 100%

Then play/scrub the clip and confirm the Cloudy 01 hat is visible during editor playback/render.
