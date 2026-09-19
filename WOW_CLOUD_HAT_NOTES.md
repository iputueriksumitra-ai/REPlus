# WOW TEAM SCENE — Rockstar Editor+ Cloud Hat Extension

Purpose: restore explicit GTA Cloud Hats (for example `Cloudy 01` and `Nimbus`) during FiveM Rockstar Editor playback/render when the `.clip` kept the weather but did not retain the runtime Cloud Hat.

## Added UI

Top-level marker menu:

- Rockstar Editor+ -> Scene Clouds
  - Cloud Hat
    - As Recorded
    - altostratus
    - Cirrus
    - cirrocumulus
    - Clear 01
    - Cloudy 01
    - Contrails
    - Horizon
    - horizonband1
    - horizonband2
    - horizonband3
    - horsey
    - Nimbus
    - Puffs
    - RAIN
    - Snowy 01
    - Stormy 01
    - stratoscumulus
    - Stripey
    - shower
    - Wispy
  - Cloud Opacity: 0%..100% in 5% steps

## Important implementation rule

`LOAD_CLOUD_HAT` is event/state driven. It is NOT called every frame.
The ScriptHookV fiber only observes state every frame; native writes happen only when needed.
This preserves the natural cloud-motion behavior established by WOW TEAM SCENE V66/V67.

## Config keys

```ini
OverrideCloudHat=0
CloudHatType=4
CloudHatOpacity=1.0
```

`CloudHatType=4` is `Cloudy 01`.

## Test

1. Record a FiveM clip with WOW TEAM SCENE weather CLOUDS + Cloudy 01 (or Nimbus).
2. Open Rockstar Editor with this RE+ build.
3. Open the top-level marker menu -> Rockstar Editor+ -> Scene Clouds.
4. Set Cloud Hat to `Cloudy 01` or `Nimbus`.
5. Leave Cloud Opacity at 100% initially.
6. Play/scrub the clip, then export a short test.

The extension does not modify the `.clip` file.
