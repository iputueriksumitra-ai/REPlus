# Rockstar Editor+

Camera and rendering mod for GTA V's Rockstar Editor. Legacy (`GTA5.exe`) and
Enhanced (`GTA5_Enhanced.exe`), singleplayer and FiveM, one build.

**1 — Camera and scene**

- **Spline camera** — markers joined by a curve through every point instead of
  the stock straight line. Position, rotation and FOV. Attached markers curve in
  the parent entity's frame; look-at markers re-aim from the splined position.
- **Procedural shake** — six-axis, per marker, in degrees and hertz.
  Deterministic: same clip, same output.
- **Time of day and weather** — relight a clip at any hour, or replace the
  weather it was shot in. A clip normally replays its own clock, weather *and*
  lighting, so none of it can be changed; these are substituted as the frame
  plays and nothing is written to the `.clip`.

**2 — Engine fixes**

- **No stall between every action** — on a modded install the editor pauses for
  seconds after every seek, cut and marker move. It is waiting for the whole
  game's streaming to fall idle, which a modded game never does, so it waits out
  its give-up timer instead. That wait is dropped.
- **Detail follows the camera** — the world streams around the *player*, so
  flying the free camera anywhere leaves you filming a scene that is still being
  loaded for someone standing where you took off.
- **Longer recordings** — the clip length limit is a memory budget, not a timer,
  which is why a busy street gives you seconds where an empty road gives a
  minute. Lifting it takes ~17 seconds of dense city to **1m 45s**. On by
  default; costs ~600 MB of reserved address space.
- **Free camera on first-person clips** — the editor normally greys out the whole
  Camera submenu on anything recorded in first person, leaving the clip stuck
  with the recorded view.
- **The rest of the guard rails** — camera distance, collision, zoom range,
  profanity check.

**3 — Rendering**

- **Render pipeline** on the Export button — video or image sequence, arbitrary
  frame rate, accumulated motion blur, project audio, no watermark. Renders every
  clip in the project.

---

---

## Requirements

| | |
|---|---|
| GTA V | Legacy or Enhanced. Singleplayer or FiveM |
| ASI loader | any |
| ReShade | **rendering and DOF only**, must be the build **with full add-on support** |
| `IgcsConnector.addon64` | **the bundled copy** (our fork) — capture bridge, rendering and DOF only |
| `ffmpeg.exe` | bundled — video output only |

Camera and shake need only the ASI loader.

ReShade is required for rendering because an ASI cannot read the GPU frame
buffer; the add-on does that. The ordinary ReShade build cannot load add-ons at
all — `IgcsConnector.addon64` is then ignored and Export falls back to the game's
watermarked encoder with no indication why. ReShade itself is not bundled.

**Use the bundled `IgcsConnector.addon64`, not one from anywhere else.** It is
our own fork of IGCS Connector, maintained at
[crxhvrd/simplecamera](https://github.com/crxhvrd/simplecamera) and built to
work with these mods rather than as a general-purpose add-on. It carries the
shared-memory capture channel this mod drives, the depth-of-field changes the
renderer depends on — external clock stepping, per-sample indexing, world
autofocus — and the **Copy to keyframe** button. Upstream IGCS Connector has
none of that: it will load, present happily, and never capture a frame.

The **Rockstar Editor+** row on the Export screen states both requirements and
tells you which one is currently unmet — no ReShade, the wrong ReShade build, or
the right build with an add-on that is not presenting.

### FiveM

Supported. Same build, `plugins\` folder, nothing to enable. Startup routes
through ScriptHookV there (the usual deferred-init detour cannot be placed in
FiveM's address space); behaviour is otherwise identical.

Two constraints. FiveM refuses any plugin that does not declare the running game
build — the shipped ids cover every build it currently offers. And an older build
loses whichever features its patterns no longer match, keeping the rest: every
address resolves independently and each hook is guarded, so a miss disables that
one feature and says so in the log. On b3258, for instance, world-collision
disable and the profanity bypass are inert while the spline, shake, menu,
distance leash and export all work. Builds 3751 and 3889 are tested.

---

## Installation

Extract into the game folder:

```
<game>\RockstarEditorPlus.asi
<game>\RockstarEditorPlus\                     settings, presets, ffmpeg
<game>\IgcsConnector.addon64                   capture bridge (ReShade add-on)
<game>\reshade-shaders\Shaders\IgcsDof.fx      depth-of-field shader
```

No configuration needed before first launch. The `.ini` files are shipped rather
than generated so you get the commented versions.

For rendering, additionally install ReShade **with add-on support**. The bundled
`ffmpeg.exe` takes precedence over any on PATH, so renders encode identically on
every machine.

---

## Menu

All settings are in the editor. Open a marker's menu and find the
**Rockstar Editor+** row.

| Where | Behaviour |
|---|---|
| Top-level marker menu | pages through global settings: **Curve**, **Limits**, **Scene**, **Scene Lights**, **Scene Clouds** |
| Camera submenu | group switcher: Spline, **Depth of Field**, Shake, Shake Motion, 4 advanced pages |
| **Export screen** | the renderer's own settings, under the stock Frame Rate and Bit rate |

Pages rather than one long list because the editor's Scaleform column draws 16
rows and silently discards the rest.

The Export screen carries thirteen extra rows — **Rockstar Editor+**, then
Output, Encoder Preset, Frame Rate, Capture Mode, IGCS Depth of Field, Aperture,
Bokeh Quality, Autofocus, Motion Blur, Shutter, Highlight Boost and Audio. That
fills the column exactly: it draws sixteen and the screen carries three stock
rows. Turning the renderer
on greys out the game's own Frame Rate and Bit rate, which configure an encoder
that will not run; turning it off greys ours instead. Either way the panel on the
right explains the highlighted row, including a greyed one — so the game's own
Frame Rate says that the export has been handed over rather than just sitting
there dead. Everything there is the same setting as the matching key in
`Render.ini`, and a change made in the menu is written straight back to it.

**Adjust Step** sets the increment for left/right, 0.001 to 1. Changes save
automatically — global to the ini, per-marker to a side-car file.

---

## 1 — Camera and scene

How the shot moves and how it is lit: the curve between markers, the shake on
top of it, and the time of day and weather it plays in. Everything here is
authored per project — the mod's own settings, not the game's.

### Spline

On by default; takes over markers set to **Smooth** blend.

| Setting | |
|---|---|
| Spline Path / Rotation / Zoom | which channels the curve drives |
| Curve Shape | Uniform / Centripetal / Chordal — tension |
| Speed Profile | Natural / Continuous / Per Segment — pacing |
| Camera Weight | Off … Full — how much mass the camera has |

**Read this first: the path is one curve through every marker.**

It is not a set of straight lines joined at the markers. That is the whole point
— straight lines meet at a corner, and a camera that turns a corner instantly
looks like a cursor, not a camera.

The consequence surprises people, so it is worth stating outright:

> **A marker's direction is decided by the markers either side of it, not by the
> segment you are looking at.**

At any marker the camera travels roughly parallel to the line joining its
*previous* and *next* markers. So if you place three markers meaning "go
straight, then turn right", the camera does not go straight and then turn — it is
already turning as it passes the middle marker, and the run up to that marker
**bows slightly left** to set the corner up.

That is the "it drifts left before going right" everyone reports. Nothing is
broken and no setting is wrong: it is the same line a car takes through a bend,
and it is why the movement reads as camera work rather than as a mouse cursor.
The camera still passes exactly through every marker — only the route *between*
them curves.

It is worth knowing how much. Three markers at `(0,0) → (10,0) → (20,10)`, a
10 m straight into a 45° turn, measured on the shipping defaults:

> the "straight" leg bows **0.74 m** to the wrong side, worst two thirds of the
> way along, and the camera is already **26° into the turn** as it passes the
> middle marker.

Under **Continuous** or **Per Segment** pacing, Curve Shape pulls that in:

| Curve Shape | wrong-way bow | angle at the marker |
|---|---|---|
| Uniform | 0.74 m | 26° |
| Centripetal | 0.57 m | 22° |
| Chordal *(default)* | **0.43 m** | **18°** |

Under **Natural** pacing — the shipping default — none of those apply and the
bow sits at the Uniform figure. What moves it there is *when* the markers are,
which is the next section.

**Marker timing changes the shape of the path, not just the speed.**

This one surprises people who already understand the curve, so it is worth its
own heading. Under Natural pacing the curve is parameterised by marker **time** —
the knots are the marker timestamps, not the distances between them. Drag a
marker along the timeline without moving it an inch in the world and *the route
changes*.

Same three markers as above, same geometry every time, only the timing moved:

| Marker timing | wrong-way bow |
|---|---|
| evenly spaced | 0.74 m |
| first leg held 4× longer | **1.19 m** |
| second leg held 4× longer | **0.30 m** |

So a marker you linger on throws a wider curve, and one you rush through pulls
the path tight. If a corner swings too wide, shortening the leg *into* it is
often a better fix than touching any setting — and it costs nothing, because you
were going to time the shot anyway.

This is specific to Natural pacing. Continuous and Per Segment parameterise by
distance instead, so the *route* stops listening to timing — all three rows above
measure 0.74 m under those modes. Marker times still set how fast the camera
covers that route; they just no longer bend it.

**If you want it to go straight and then turn, say so with markers.** A spline
can only be told where to be, so add a marker where you want it held. Two markers
close together on the straight section pin the path there and leave the turn to
happen after them. This is not a workaround; it is how you steer any curve
editor, and it is the same reason a Bezier in After Effects or Blender bows
between keys.

Other ways out, in the order worth trying:

| Want | Do |
|---|---|
| the path pinned somewhere specific | **add a marker there** — works under every pacing mode, and is almost always the right answer |
| one corner tightened | **shorten the leg into it** on the timeline — free, and the default pacing is the only one that listens |
| tighter corners everywhere | **Curve Shape** → Chordal — but only after switching **Speed Profile** off Natural, or it does nothing |
| one segment perfectly straight | that marker's **Spline Path** → **Stock** |
| straight lines everywhere | **Spline Path** off globally — back to the stock blend |

The first and last markers have no neighbour to take a direction from, so the
curve reflects one. If a shot starts by drifting the wrong way, that is why —
add a marker before the one you actually want to start on.

None of this is affected by **Camera Weight**, which is off by default and is a
separate thing: the curve above passes through every marker exactly. Weight is
what makes it stop doing that.

**Speed Profile**

| | |
|---|---|
| **Natural** (default) | path driven from marker times, no arc-length correction. The curve's own parameter speed carries the camera — eases into tight bends, flows through open ones |
| Continuous | one speed curve fitted across all markers. Even and metered |
| Per Segment | each segment paced independently |

**Curve Shape** — how tightly the curve is pulled around each marker. Uniform
gives long tangents and wide turns; Chordal (default) spaces knots by chord
length; Centripetal sits between them.

Centripetal is the usual textbook recommendation because it provably never
overshoots between markers. That guards against the wrong failure here: it
shortens the tangent wherever a neighbouring marker is close, which at a
**reversal** leaves the camera no momentum to sweep around, so it overshoots the
marker and hooks back across its own path. Measured on a 160° reversal, distance
travelled over distance covered was **1.11 at Uniform, 1.56 at Centripetal, 3.59
at Chordal**. If a shot loops at a marker it doubles back through, try Uniform.

With evenly spaced markers that never reverse, all three are indistinguishable.

> **Curve Shape has no effect under Natural pacing**, the default — a
> time-parameterised Hermite has no knot spacing to choose. Switch to Continuous
> or Per Segment for tension to apply.

**Camera Weight**

Catmull-Rom passes through every marker exactly, which reads as a weightless
cursor snapping to coordinates. Weight blends the curve toward a B-spline, which
is *carried past* its control points the way a rig with mass is — the markers
become where you steered, not where the camera went.

It moves **position, aim and zoom** together. Aim matters most: a heavy head has
to be swung, and a body that lags while the aim snaps just drifts the subject in
frame.

The offset at a marker is one sixth of the second difference, scaled by the
knob — so it is **corner-selective**. A 90° corner with 10 m legs passes about
2.4 m inside the marker at Full. On a straight or gently curving run there is
almost no second difference and Weight does nothing, which is right: mass only
fights you when you change direction.

> Applies **while playing only**. Parked, the camera sits exactly where you put
> it, so what you frame is what the marker records.

A B-spline undershoots but never overshoots — it cannot leave the region bounded
by the markers. Real mass also carries *past* when the steering stops; that half
needs a stateful spring, which could not be re-rendered deterministically. If a
shot wants overshoot, place the marker beyond the target and let the undershoot
land it.

Per marker: override curve shape, add ease-in/ease-out, or force stock behaviour
for one segment.

### Shake

Set a marker's stock **Shake** row to **Rockstar Editor+**.

**Simple mode** (default) — two real units, shared with the sibling Simple
Camera project, so values transfer between them.

| Setting | |
|---|---|
| Shake Amplitude | degrees of rotation; 5 cm of translation per degree |
| Shake Frequency | hertz. 0.35 ≈ a three-second wander, 0.20 slower |
| Shake Variation | swell and settle over time. 0 = statistically flat |

Handheld starting point: **amplitude 2.0–2.5 at frequency 0.20**.

**Motion coupling** — off by default. `Motion → Intensity` at about 1.0 is the
single biggest step towards a shake that reads as an operator rather than an
effect. `Motion → Speed` scales rate the same way; `Stop When Still` fades out
when the camera is parked.

**Complex mode** drives the two layers directly — slow **Sway**, fast
**Jitter**, each with movement, rotation and rate — plus per-axis weights on all
six degrees of freedom.

**Apply Shake to All** on any shake page copies that marker's shake to every
marker in the project. Shake only; curve and easing are untouched.

> Shake is a function of clip time, so it is frozen while the playhead is.
> Play or scrub to judge it.

### Time of day and weather

**Rockstar Editor+ → Scene**, in the top-level marker menu. Ships off.

A clip records the clock and the weather on every frame and replays them, which
is why setting the time or forcing weather from a trainer does not survive a
single frame in the editor. These rows substitute different values as the frame
is played instead, so the change holds for the whole clip and shows up live.

| Row | |
|---|---|
| **Timecycle** | the page's master switch. **Live** re-lights the clip for the settings below; **As Recorded** keeps the clip's own lighting and greys the rest out |
| **Time of Day** | As Recorded, or any quarter hour. Relights the whole clip |
| **Weather** | As Recorded, or one of the game's 15 types |
| **Weather Blend To** | a second type to sit between |
| **Weather Blend** | 0–1 between the two. This is the engine's own transition, so half-way is real weather rather than a cross-fade |
| **Wetness** | wet roads and puddles, independent of the type. As Recorded is not the same as 0 — a clip shot in the rain has wet ground |

Weather is handed to the game's own weather code rather than forced field by
field, so clouds, wind, puddles and the snow effects all follow the type you
pick.

**Why the Timecycle row exists.** A clip does not just record the clock and the
weather — it records the *fully resolved timecycle keyframe*, every lighting
variable already evaluated for the hour and weather it was shot in. The engine
still evaluates the timecycle live on every replayed frame, then throws it away
variable by variable and puts the recorded one back. So on its own, moving the
clock to midnight moves the sun and leaves an afternoon sky; switching to RAIN
gives you rain particles over sunshine. **Live** stops that replacement, so the
freshly evaluated lighting survives.

The trade is that any timecycle *modifier* the clip carried goes with it — an
interior grade, a mission colour grade, script post FX. Set the row to
**As Recorded** to keep those and accept the baked lighting.

That is why Timecycle is the master switch rather than one setting among six:
with the recorded lighting standing, a new time of day would only swing the sun
across an unchanged sky and a new weather would drop rain into sunshine. On
**As Recorded** the other rows are greyed *and* inert — the clip plays exactly
as shot, and what the rows say matches what playback does.

> **Nothing is written to the `.clip`.** The substitution happens as the frame
> is played, so the file on disk is untouched and a clip opened without the mod
> is exactly as it was shot. That also means the look is a setting rather than
> an edit: set a row back to *As Recorded* and it is gone.


### Scene Clouds — WOW TEAM SCENE extension

**Rockstar Editor+ → Scene Clouds** adds a runtime Cloud Hat override for FiveM
Rockstar Editor playback and export. This exists because a GTA Cloud Hat is a
separate runtime layer from the weather packet and is not reliably serialized
into a `.clip`. A clip can therefore retain `CLOUDS` weather while losing an
explicit `Cloudy 01` / `Nimbus` hat when it is opened in the editor.

| Row | |
|---|---|
| **Cloud Hat** | **As Recorded** does nothing; otherwise forces one of the same GTA Cloud Hat names used by WOW TEAM SCENE, including `Cloudy 01`, `Nimbus`, `Wispy`, `Horizon`, `Puffs`, and the other stock hats |
| **Cloud Opacity** | 0–100%, written only when the requested value changes |

The bridge runs on FiveM's ScriptHookV game fiber. Its heartbeat only watches
for editor entry/exit and setting changes: **`LOAD_CLOUD_HAT` is not called every
frame**. This is deliberate. WOW TEAM SCENE V66/V67 testing showed that repeated
cloud/time state writes can disturb natural cloud motion on graphics mods.

When the override is disabled or the editor is left, the extension unloads the
hat it forced and restores the cloud alpha captured before the override. The
`.clip` file is never modified.

### Scene lights

Free-standing point and spot lights placed anywhere in the world — the editor
has none of its own, so a night shot is lit by whatever the map already has.

**Rockstar Editor+ → Scene Lights** in the marker menu. Add a light, then:

| | |
|---|---|
| **Grab** | the camera flies to the light and carries it while you move. Release and the camera returns to the shot you had |
| **Place** | drops the light where the camera is and leaves it there |

Colour, intensity, range and falloff for both types; cone angles for a spot;
volume settings for a light meant to be seen in fog. A spot faces down its beam
when grabbed, so you are looking at what it lights.

**Shadows** are opt-in per light. Worth knowing before you wonder why one does
not cast: the engine gives a shadow map to **eight lights in a scene**, scored
by range over distance. In a busy street a scene light competes with every
headlight and street lamp around it and can simply lose. Raising *Range* is the
lever that wins a slot.

**Two sets, and which one you are editing depends on where you are.** Inside a
clip the lights belong to that clip and are saved with the project, under
`lights\scenelights.txt`. Outside it, `RockstarEditorPlus\Lights.ini` is the
set — and it is also the default every clip you have never lit starts from.
Editing a clip's lights never writes back to it.

That file is re-read every two seconds, so lights can be added, moved and
retuned by hand without restarting the game.

> Nothing ships enabled. The game consumes the light list on every frame it
> renders, not only in the editor, so a light in that file lights free roam
> too — which is the point if you want it, and a surprise if you do not.

---

### Per-marker settings

A marker's curve, shake and per-axis values are stored beside the project rather
than inside it, because the marker struct is serialised straight into the `.clip`
file and has nowhere to put extra fields without breaking the format the game's
own loader validates. Keeping them outside means a project still opens on a
machine without this mod.

They are scoped to the **project and clip** you set them in.
`markers\<project>.txt` holds one project, with its clips as sections inside:

```
RockstarEditorPlus v7
seen 1755530000
clips 2
clip 4a1f9c2e7b0d3856
marker 7138 shake=1 intensity=1.35 freqMul=0.15
marker 7968 shake=1 intensity=1.35
clip 0e83b5d1a2c4f760
marker 0 orient=2
```

Fields are named and only written when set, so the file stays readable and a
lightly-edited marker is one short line. Plain text, safe to hand-edit if a
project needs rescuing.

**Clips are identified, not numbered.** The `clip` line is the clip's identity —
the recording it came from, plus which use of that recording it is. That is what
makes editing a project safe:

| you do this | settings |
|---|---|
| delete a clip | its settings go with it, the others are untouched |
| reorder clips | follow their clips |
| trim a clip | stay with it |
| rename a clip | stay with it |
| add the same recording twice | the two get separate settings |

Scoping was by clip *index* before v7, and that broke on exactly the common case:
delete clip 2 of 5 and clips 3, 4 and 5 shift down a slot, so every marker in
them inherits the settings of the clip that used to sit there. One deletion, four
broken clips.

A v6 file is converted the first time its project is opened under a build that
reads identities. If it refers to a clip the project no longer has, it cannot be
matched and is copied to `<project>.txt.orphan` rather than being deleted — the
log says so, and the values are still in that file if you want them back.

> The one case identity cannot separate is reordering two clips cut from the
> **same recording** with nothing else to tell them apart. Nothing available to
> the mod distinguishes those, so they are matched in order.

---

## 2 — Engine fixes

Two kinds of thing, both in the way of the same job. Some are guard rails — the
editor is built for clips, not for shots, and most of what stops you is a policy
rather than a technical bound. The rest are engine behaviour that is simply wrong
for an editor: waits that never end, and a world that loads around the player
while you are filming somewhere else.

Each is lifted independently and each is a single ini key, so any of them can go
back to stock on its own. Every one resolves separately too: if a pattern does
not match on your build the mod disables **that** one and says so in the log,
keeping the rest.

### The stall between every action

`FastPrecache=1` (default), `FastPrecacheAudio=1`, `PrecacheMaxMs=1500`,
`UrgentModelLoadMs=250`, `FastPrecacheDuringRender=0`.

Seek, cut, or nudge a marker on a heavily modded install and the editor freezes
for seconds with the controls locked. Everyone assumes memory, and every heap,
pool and VRAM adjuster ever pointed at it has failed to help.

It is not a capacity problem. After every seek the editor precaches, and stock
code will not continue until the number of outstanding streaming requests **for
the whole game** has been zero for ten consecutive frames. A modded game never
reaches zero — the scene streamer rescores what is visible every frame, and the
replay's own preloader adds requests inside the same loop — so the wait always
runs to its give-up timer instead. Nothing you can allocate makes an idle
requirement true, which is exactly why more memory never fixed it.

Worse, that timer is counted in frames capped at 33 ms each, so it is a *frame*
budget wearing a millisecond label: at 200 fps its 6600 ends up meaning 200
frames. High frame rate makes the stall longer, not shorter.

The mod drops the whole-game idle requirement and keeps the wait that actually
matters — the replay's own ±4 s preload — under a ceiling. Two smaller waits go
with it: a separate audio one, and a blocking model load that stock gives 5000 ms
to finish. Everything here is gated on the editor being open, so gameplay,
recording and clip loading are untouched.

Renders keep the stock wait by default, since a render has time and would rather
have the frame complete.

### Recording length

`ReplayBlocks` in `RockstarEditorPlus.ini`. The recording ring **is** the clip:
it fills, the clip saves, and recording rolls straight into a new one — so
length is ring size divided by how fast the scene fills it. That is why the
number moves with traffic density rather than being a fixed duration.

The game ships 30 blocks, and not by preference: its settings code force-clamps
the count to 30 whenever the replay heap is under ~196 MB, and the heap is
172 MB. The heap is widened first, so the clamp stops firing.

Measured in dense city traffic:

| | ring | clip |
|---|---|---|
| 30 *(the game's own)* | 120 MB | ~17 s |
| 64 | 256 MB | ~44 s |
| **128** *(default)* | 512 MB | **~1m 45s** |

**It costs RAM.** Each block needs 4 MB plus a 384 KB thumbnail, so the default
reserves about 604 MB for the session, and clips grow to match — a 128-block
clip is roughly 230 MB on disk. That is the right trade for a tool whose whole
job is capturing footage, but lower it if memory is tight. If you shoot long,
check the editor's own disk allowance under Settings → Saving & Startup.

Above 30 depends on the heap widening succeeding. If it cannot — the patterns
did not resolve on your build, or the process cannot reserve that much address
space — the value is clamped back to 30 and the log says which. Takes effect at
the next recording, so restart before shooting.

**Under FiveM this has to happen much earlier.** The pool is sized once at
startup and can only be changed before that, and FiveM defers the mod's init to
ScriptHookV — 20 to 90 seconds later, long after the pool is committed. So the
widening runs at DLL attach instead, before anything else. Same result; the log
reports it separately (`replay heap 172 -> 604 MB ... patched at attach`) so you
can tell which path did the work.

### First-person clips

Record anything from the first-person view and the editor greys out the whole
Camera submenu — the clip keeps the recorded view forever.

`UnlockCameraRestrictions=1` (default) removes it, and those clips take a free
camera like any other.

There are two locks, not one. The menu greys the row, and separately the camera
director ignores whatever the marker asks for and forces the recorded camera —
so unlocking only the menu gives you a free camera that will not move, because
there is no free camera. Both are lifted.

The same pair of locks covers clips recorded during a cutscene or with camera
movement disabled, and those are lifted too. In practice you are unlikely to have
such a clip: recording stops while controls are disabled, which is the same
condition that flags them.


### Camera distance

`UnlimitedCameraDistance=1` (default), `MaxCameraDistance=20000`.

Stock leashes the free camera to **30 m** from the player and yanks it back —
the same value feeds the out-of-range warning and the fallback to the recorded
camera, so lifting it once covers all three.

Stock also streams the world around the *player*, not the camera, which is the
other half of why the leash exists — see **Streaming focus** below, which
removes it.

### Streaming focus

`StreamingFocusOnCamera=1` (default). Menu: **Detail Follows → Camera**.

Everything that decides what the engine keeps at full detail centres on one
point, and stock that point is the player ped: map data and IPL cull boxes, the
HD/LOD scene streamer, static collision, where peds and vehicles are populated,
and which peds get full AI and animation. Fly out a few hundred metres and the
world you are pointing at is still being streamed for someone standing where you
took off — low LOD, missing map, and peds and traffic that thin out around the
shot.

This moves that point onto the editor camera, so detail arrives where you are
looking. Pair it with `UnlimitedCameraDistance`: a camera that can go anywhere
and a world that only exists in one place is half a feature.

It is the game's own mechanism, not a patch — the player-switch camera and
Rockstar's internal debug free camera ask for the same thing, through the same
flag on the rendered camera frame. Nothing is left behind when it goes off
either: the engine restores the player-ped focus by itself on the first frame
the flag is absent.

The cost is honest streaming work. Whipping the camera across the city makes the
streamer fetch the map along the way, the same as driving it would.

### Camera collision

`DisableCameraCollision=1` (default).

Stock pushes the camera off geometry. Pushing through a doorframe, a windscreen
or a fence is most of what a cinematic move is, and the push-off does not just
block those — it silently bends the path away from the markers you placed, so
the mod would be fighting its own spline.

The cost is that you can end up inside solid map with no visual reference. That
is recoverable in a frame; a shot the push-off quietly ruined is not obvious at
all.

### Zoom range

`UncapZoom=1` (default), `ZoomMinFov=1`, `ZoomMaxFov=130`.

Stock allows a 10x span, 0.45x to 4.50x. That is a gameplay-camera choice, and
this is not a gameplay camera — a long lens is ordinary cinematography and the
range is the first thing anyone runs into.

The ends default to the engine's own clamp, 1° to 130° of FOV, so nothing
outside them was ever reachable. The editor's readout is `45 / fov`: 1° reads as
45x in, 130° as 0.35x out.

### Profanity check

`BypassProfanityFilter=1` (default).

Naming or exporting a project polls Social Club, and the poll times out to a
refusal when it cannot be reached — so offline you cannot name a project at all,
which is a network check standing between you and a local file.

---

## 3 — Rendering

A complete replacement for the editor's own export, driven from the same button:
arbitrary frame rate, true accumulated motion blur, project audio, no watermark,
every clip in the project.

### The renderer

Triggered by **Export**. Nothing else starts it.

**Ships off** — switch it on with the **Rockstar Editor+** row on the Export
screen, or set `EnableRenderer=1` in `Render.ini`. It is opt-in because it
is the one feature that cannot coexist: diverting the bake means other export
tools (EVE, EVER) never fire. They do not error, they silently never run.
Everything else the mod does is additive.

**Output** — `RenderMode=Video` (default) hands frames to ffmpeg as they finish
and deletes them, so a long render costs a couple of files at a time rather than
thousands; project audio is recorded and muxed from the same Export press.
`RenderMode=Frames` writes a numbered PNG/JPEG sequence with an `assemble.txt`
of ready-made ffmpeg command lines. Audio works here too — it lands as
`audio.wav` beside the frames and `assemble.txt` carries the arguments that mux
it onto whichever conform you pick.

The capture is identical either way. Codecs come from `presets\`, selected with
`RenderVideoPreset=h265`, or write ffmpeg arguments directly into
`RenderVideoArgs`. Edited presets persist; a deleted one is rewritten.

Twenty-one ship, in five groups:

| | |
|---|---|
| **Delivery** | `h264` `h264_upload` `h265` `av1` `vp9` |
| **Delivery, GPU** | `nvenc_h264` `nvenc_hevc` `nvenc_hevc_10bit` `nvenc_av1` |
| **Editing** | `prores_hq` `prores_4444` `dnxhr_hq` `dnxhr_444` `cineform` |
| **Lossless** | `utvideo` `magicyuv` `ffv1` `lossless` `qtrle` |
| **Share** | `webp` `gif` |

`utvideo` is the one to reach for if you used **Lagarith** elsewhere — ffmpeg
can read Lagarith but has no encoder for it, and Ut Video is the same idea,
faster, with a free VfW codec so editors read it off the timeline. `ffv1` is
about a third smaller for the same bit-exact result but scrubs badly; `lossless`
(x264 at qp 0) is smaller still and slower again.

Every delivery and editing preset tags colour as BT.709 and converts with
`+accurate_rnd+full_chroma_int`. The renderer hands over RGB frames, so anything
YUV is a conversion, and an untagged file leaves the player guessing which
primaries it used — players guess 601 often enough that every hue shifts.

**Motion blur** — 64 samples at a 360° shutter by default. Every sample is a
real render at a real instant, so this is true accumulation, not a screen-space
smear. Samples cost render time and nothing else, and 64 is genuinely expensive
— 64 captures per output frame. Drop `RenderSamples` to 3 while setting a shot
up. `RenderShutter=0.5` is the 180° film convention.

**Audio** — `RenderAudio=1` (default). One Export press does two passes: the
project plays through once at normal speed to record sound, then rewinds to clip
one and renders frames. In `Video` mode it is muxed at the end; in `Frames` mode
it is left as `audio.wav` next to the sequence and `assemble.txt` tells you how
to attach it. Alternatively point `AudioFromFile` at a stock export of the same
project.

**Duration** — rendering seeks frame by frame and is far slower than real time.
Progress and an estimate go to the log every 15 seconds.

### Capture modes

The **Capture Mode** row on the Export screen, or `RenderCaptureMode` in
`Render.ini`. Both average `RenderSamples` real renders into every output frame —
the difference is what the world does between them.

| | Sliding *(default)* | Walking |
|---|---|---|
| the clip | steps forward | paused, seeked per sample |
| particles, TAA, SSR, RT | keep simulating | reset at every sample |
| **live cloth** (flags, ropes) | **runs far too fast** | **frozen** |
| shutter | exact | exact midpoints |
| frames per output frame | `samples / shutter` | `2·samples + 2` |
| 64 samples @ 360° | **~1.6 s/frame** | ~4.6 s/frame |

> **Live cloth does not render correctly in either mode.** Flags whip, ropes
> race, or everything hangs dead still. This is an engine limitation rather than
> something the renderer chooses — see [Cloth and other live physics](#cloth-and-other-live-physics)
> below before planning a shot around a flag.

Sliding advances to each frame's mark, then exposes N consecutive frames with
the world still running. That keeps anything with temporal history warm, which a
seeked frame cannot — every walking sample starts cold.

It is also faster at the default 360° shutter, because it spends one frame per
sample where walking also needs a settle frame. **That edge is shutter-dependent**
— it needs `samples / shutter` frames, so at 0.5 the two are level and below that
walking wins.

Walking is still the more predictable one: exact shutter placement, deterministic
frame times, and it fails obviously — a repeated frame rather than a smeared one.
Reach for it if a sliding render looks wrong, or for short shutters.

Sliding does not ask the game to play slowly and then measure where it landed.
It drives the replay's own **fixed-time export** stepper — the one Rockstar's
video export uses — which advances the clip by an exact duration accumulated in
integer nanoseconds. So the shutter is exact rather than approximate, there is
no calibration to get wrong, no minimum step to fall under, and nothing that
depends on your frame rate.

### Cloth and other live physics

Flags, ropes and character cloth are **simulated live**, not played back. The
editor replays recorded positions for everything else, but cloth is integrated
on top of that as the frames are drawn — which is why it is the one thing that
does not survive a render intact.

The cloth is a Verlet integrator. Its velocity is implicit in the gap between
the last two positions, and nothing rescales that gap when the frame timing
changes, so **cloth advances once per rendered frame rather than once per unit
of game time**. That single fact produces both failures:

| | presents per output frame | result |
|---|---|---|
| Walking | replay paused, none | cloth never steps — **frozen** |
| Sliding | `RenderSamples`, so 64 by default | 64 steps per frame — **~64× too fast** |

Neither is one step per output frame, which is the only value that would be
right. Halving `RenderSamples` roughly halves the speed-up, which is the quickest
way to confirm you are looking at this and not something else.

**It is not specific to the renderer.** The same thing happens in ordinary play:
put the game in heavy slow motion and cap the frame rate, and cloth stops obeying
the slow motion somewhere around 33-34 fps — more frames per game-second, more
integration steps, faster cloth. The renderer only makes it obvious, because 64
samples is a far bigger multiplier than a frame-rate cap.

There is no fix available from this side. The repair would be to step the cloth
once per output frame, and the engine takes one global timestep for everything —
so the single step it took would carry a *sub-sample* dt, about 1/64 of a frame,
and gravity would be under-applied by the square of that. The flag would move at
the right speed and hang limp.

**In practice:** avoid putting flags, banners, ropes or loose clothing where they
carry a shot. If something cloth-bound is unavoidable, Walking at least fails
predictably — a still flag reads as a windless day, where a whipping one reads as
broken footage.

> Vegetation may be a separate problem. Tree and grass wind is a vertex-shader
> effect driven by a clock the engine hands to the shader, not an integrator, so
> it cannot be sped up by taking more samples — a stateless `sin(phase + t)`
> gives the same answer however often it is asked. If trees also look fast in a
> render, that points at the clock feeding them rather than at anything here, and
> it is unconfirmed either way.

**Rain was a separate problem, and a real bug.** It gets its own note because it
looked like the cloth one and was reported as it, and the two have nothing in
common. Weather particles are *dt-driven* — the GPU drop shader advances by the
frame's time step, not by one step per frame — so they failed in the opposite
direction: too **slow**, not too fast, and only in a **depth-of-field** render.
The cause is not the amount of time — it is the *size of each step*.

An aperture sweep divides one frame among hundreds of samples, so at the default
quality each present advances the clip by about **0.06 ms**. The engine's GPU
weather particles cannot integrate a step that small. `PtFxGPUManager.cpp` says
so in as many words — *"we use half floats for velocity and position, we get
precision issues when the time delta goes below a small value"* — and ships
`0.009f`, **9 ms**, as "the minimum value that works correctly". The sweep was
handing it a step 150× below that, so the drops barely advanced while everything
replayed from the recording moved perfectly normally.

The giveaway is that rain does not run at a steady fraction of its speed — it
starts nearly **frozen and accelerates**, over about ten frames at 48 samples and
over several seconds at 800. That is rounding on a running sum: each step adds
gravity to the drop's velocity, and once that addition falls below the precision
of the number it is being added to, it is silently dropped. Velocity climbs until
it hits a ceiling set by the step size — roughly 1 m/s at 685 samples a frame,
about 17 m/s at 48, and no ceiling at all with the two steps a plain sliding
render uses. Rain falls at around 9 m/s, which is why one looks broken, one looks
sluggish at the start, and one looks right.

The other tell, and it is worth remembering for any similar report:
peds, vehicles and the camera are *replayed from recorded positions*, so they
land correctly however the frame's time is subdivided. Only what the engine
**integrates live** — rain, mist, cloth — can be got wrong this way.

**This is an engine limit, and it is not going to be fixed here.** Two repairs
were built and both were abandoned. Coarsening the clock into 9 ms steps works
for the particles and destroys the motion blur, which is the whole point of the
feature. Leaving the clock alone and scaling only what the particles integrate —
holding them still through the exposure, then handing them one whole frame at
once — is sound in principle, was implemented against the right field in both
builds, and still did not put rain back to normal speed on screen.

At that point the honest conclusion is that the engine is being asked to render
several hundred frames per unit of game time, and its live-simulated systems were
never built for it. There is no setting for this, because there is nothing that
usefully trades off.

**What actually helps: fewer samples.** The precision ceiling scales with the
step, so a lower ring count raises it — around 48 samples a frame is already
above rain's own falling speed, where 686 is nowhere near. A lens shot in the
rain wants a modest ring count. A dry one can afford any number you like.

The distinction worth keeping: **cloth is too fast, rain was too slow, in the
same render.** Anything that integrates a time step can be repaired from here;
anything that counts frames instead cannot.

### Depth of field

Two ways in: as a **render modifier**, which is what you want for a finished
shot, or as an interactive **session** in ReShade for setting one up.

**As a render modifier**

The **IGCS Depth of Field** row on the Export screen, or `RenderDepthOfField` in
`Render.ini`. It is a modifier, not a capture mode — it layers onto whichever
mode is set, and the two combine:

| | what a frame is |
|---|---|
| Walking + DoF | the clock is frozen for the whole sweep — depth of field only, no motion blur |
| Sliding + DoF | the clock steps *between* aperture samples — depth of field **and** motion blur from one sweep |

Sliding + DoF is the interesting one, and it costs nothing over depth of field
alone: the samples were going to be taken anyway, so spreading them across the
shutter as well as across the aperture is free. `RenderSamples` is ignored — the
aperture decides the count — while `RenderShutter` still sets the exposure.

This is the slowest thing here by a wide margin. Every output frame is a whole
aperture sweep, so a shot measured in minutes elsewhere is measured in hours.

**As a session**

The capture add-on can walk the camera around a lens aperture and blend the
result — real optical bokeh rather than a screen-space approximation.

Started from **ReShade's own overlay**, the IgcsConnector depth-of-field panel.
There is no switch in the editor. Requires:

- the editor open, on a free-camera marker
- **playback paused** — the session drives the clock itself
- `IgcsDof.fx` in the ReShade shaders folder (bundled)

**Shutter (ms)** in that panel decides what a session integrates. At **0** it is
one frozen instant: every sample is the same moment from a different point on
the lens, so only the aperture varies. That is the default and the right answer
for a locked-off shot.

Above 0 the replay clock steps per sample as well, and one pass gives depth of
field **and** motion blur — 16.7 ms is a 1/60s exposure, 33.3 ms a 1/30s one. It
costs no extra samples, so a long shutter over few samples is what discrete
ghosting looks like; raise quality alongside it.

The camera is not pinned while the clock moves, so a shot riding a spline blurs
along its path as a whole. Only the aperture's parallax is cancelled before each
sample is summed — the aperture is the lens, the spline is the camera body, and
only the first should leave the focus plane sharp.

Sample times are spread evenly across the exposure and then shuffled against the
aperture order. Both halves matter: an uneven spread beads the smear into
separate ghosts, and time that tracks aperture radius makes a moving subject
sweep radially — tight bokeh at one end of the trail, wide at the other.

**Focus, per marker**

Under **Cameras → Rockstar Editor+ → Depth of Field**, alongside the Spline and
Shake groups:

| | |
|---|---|
| Autofocus | measure focus in the world each frame, at the centre of frame |
| Focus Distance | manual focus, in the add-on's own disparity units |

Autofocus is the switch between the two: with it on, Focus Distance is greyed
out, because autofocus overrides it. It stays reachable on every marker — no
camera transition and no particular camera type required, unlike the Spline
rows in the same submenu, which grey out without a blend to act on.

Both are per marker. Focus Distance is **interpolated between markers**, so two
different values across a shot give a focus pull. A marker with nothing set
holds the previous focus rather than racking back to the default.

The pull is **eased, not linear** — smoothstep, so focus is stationary at both
markers and accelerates between them. A rack that starts and stops instantly
reads as a motor; a puller eases in and settles out. On a chain of markers that
means it settles at each one, which is what racking to one subject and then to
another should do. Markers that do not change focus are unaffected either way.

It is also linear in **disparity** rather than in distance, which is roughly
linear in dioptres — the same thing a real lens barrel does, and even in
perceived defocus. Linear in metres would hang at the near end and snap at the
far one.

**Setting one: Copy to keyframe.** Open a depth-of-field session, focus by eye,
and press **Copy to keyframe** in the add-on's panel. It writes that focus onto
the marker the editor is on and switches the marker to manual.

It converts as it copies, which is the point of it. Focus Delta is a
**disparity, not a distance** — it scales with the aperture — so a value read at
the session's Max Bokeh Size means a different plane at the render aperture.
The button rescales between the two, so they no longer have to match by hand.

You can still type a number into Focus Distance directly. If you do, the same
caveat applies: it is measured against `RenderDofBokehSize`, not against
whatever the panel was set to. Changing the aperture afterwards rescales every
stored focus value with it, so a pull keeps its shape.

> The slider only appears when the connected camera tools can step a clock,
> which outside this mod is essentially never — so on any other game the panel
> looks exactly as it did.

---

## Settings

Both files live in `RockstarEditorPlus\`, beside the `.asi`.

### `RockstarEditorPlus.ini`

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = no hooks, stock game |
| `SplinePosition` `SplineOrientation` `SplineFov` | 1 | channels the curve drives |
| `SplineAttached` | 1 | also curve shots mounted on an entity |
| `LookAtReaim` | 1 | re-aim look-at markers after the curve moves them |
| `LookAtAspect` | 0 | aspect for that re-aim; 0 = display's |
| `OnlySmoothBlend` | 1 | 0 also takes over Linear markers |
| `NaturalPacing` | 1 | overrides the two below |
| `SmoothSpeedProfile` | 1 | one speed curve across markers |
| `ArcLengthRemap` | 1 | per-segment fallback, used when both above are 0 |
| `Alpha` | 1 | tension: 0 uniform / 0.5 centripetal / 1 chordal |
| `SplineWeight` | 0 | camera mass on playback: 0 hits markers, 1 carried past |
| `ShakeSimpleMode` | 1 | 0 = Sway/Jitter model |
| `ShakeAmplitude` | 1.0 | degrees |
| `ShakeFrequency` | 0.35 | hertz |
| `ShakeVariation` | 0.7 | 0 = flat |
| `ShakeSpeedAmp` `ShakeSpeedFreq` | 0.0 | motion coupling. Try 1.0 / 0.5 |
| `ShakeStopWhenStill` | 0 | fade out when parked |
| `ShakeAxis*` | 1.0 | per-axis weights; 0 disables an axis |
| `UnlimitedCameraDistance` | 1 | lift the 30 m leash |
| `MaxCameraDistance` | 20000 | metres |
| `StreamingFocusOnCamera` | 1 | stream map, collision and peds around the camera, not the player |
| `DisableCameraCollision` | 1 | pass through geometry |
| `BypassProfanityFilter` | 1 | naming/export works with Social Club offline |
| `UncapZoom` | 1 | widen the 0.45x–4.50x range to the engine's own 1–130° |
| `ReplayBlocks` | 128 | recording length, in 4 MB blocks. 3–128 |
| `UnlockCameraRestrictions` | 1 | free camera on first-person clips |
| `FastPrecache` | 1 | drop the wait for whole-game streaming idle |
| `FastPrecacheAudio` | 1 | and its separate audio wait |
| `FastPrecacheDuringRender` | 0 | renders keep the stock wait; 1 only to A/B it |
| `PrecacheMaxMs` | 1500 | ceiling on the preload wait. 0 = stock (unbounded) |
| `UrgentModelLoadMs` | 250 | ceiling on a blocking model load. 0 = stock (5000) |
| `OverrideTimeOfDay` | 0 | relight the clip at `TimeOfDay` instead of its recorded clock |
| `TimeOfDay` | 720 | minutes past midnight. 720 = 12:00 |
| `OverrideWeather` | 0 | replace the clip's recorded weather |
| `WeatherType` | 0 | index into weather.xml's order. 0 EXTRASUNNY … 7 THUNDER … |
| `WeatherBlendTo` | -1 | second type to blend towards; -1 = none |
| `WeatherBlend` | 0.0 | 0–1 between the two types |
| `WeatherWetness` | -1 | 0–1 wet roads and puddles; -1 = as recorded |
| `LiveTimecycle` | 1 | re-light for the overridden time/weather instead of replaying the clip's baked keyframe |
| `OverrideCloudHat` | 0 | WOW extension: force a Cloud Hat while Rockstar Editor is active |
| `CloudHatType` | 4 | index into the Scene Clouds list; 4 = `Cloudy 01` |
| `CloudHatOpacity` | 1.0 | forced Cloud Hat opacity, 0–1 |
| `ShakeDebugLog` `SplineDebugLog` `SplineTraceLog` | 0 | diagnostics |

### `Lights.ini`

The default light set, and the one free roam uses. `Count` says how many
`[LightN]` sections to read — **0 by default**, so nothing is lit until you ask.
Max 32. Each section carries position, colour, intensity, range, falloff, cone
angles for a spot, and `CastShadows`.

Written back to when you edit lights outside a clip; inside one, the clip's own
set is written instead and this file is left alone.

### `Render.ini`

The thirteen rows on the editor's Export screen write straight into this file,
so the two are never out of step — the menu is the same settings with
descriptions attached. The keys below that have no row are the ones you set once.

| Key | Default | |
|---|---|---|
| `EnableRenderer` | 0 | master switch; 0 restores the stock exporter |
| `RenderMode` | Video | `Video` or `Frames` |
| `RenderCaptureMode` | Sliding | `Sliding` or `Walking` — see Capture modes above |
| `RenderDepthOfField` | 0 | render through a real aperture; layers onto either mode |
| `RenderFps` | 30 | output rate |
| `RenderSamples` | 64 | motion-blur samples; 1 = none |
| `RenderShutter` | 1 | fraction of the frame interval; 0.5 = 180° |
| `RenderDofBokehSize` `RenderDofQuality` | 0.15 / 12 | aperture width and ring count |
| `RenderDofAutofocus` | 1 | measure focus in the world each frame |
| `RenderJpeg` `RenderQuality` | 0 / 90 | JPEG instead of PNG |
| `RenderKeepFrames` | 0 | Video mode: keep frames too |
| `RenderVideoPreset` | | a name from `presets\`, or the **Encoder Preset** row on the Export screen |
| `FfmpegPath` | | empty = bundled, then beside the exe, then PATH |
| `RenderAudio` | 1 | record and mux project sound |
| `RenderHideHud` | 1 | hide the editor HUD while rendering |
| `RenderOutputFolder` | | empty = `RockstarEditorPlus\Captures\` |

---

## File layout

```
GTA5.exe
RockstarEditorPlus.asi
RockstarEditorPlus\
    RockstarEditorPlus.ini          camera, shake, limits
    Render.ini                      rendering
    Lights.ini                      default scene lights
    RE+ Render Settings.exe         editor for Render.ini and the presets
    RockstarEditorPlus.log
    ffmpeg.exe                      bundled
    presets\                        codec presets
    markers\                        per-marker settings, one file per project
    lights\                         per-clip scene lights
    Captures\render_0001\           frames, or video.mp4
```

**RE+ Render Settings** is a standalone editor for the render settings and the
encoder presets, sitting next to the files it edits.

Settings are grouped by what they do — renderer, motion blur, depth of field,
image files, output and encoding — and every one carries its own explanation,
shown on hover or on click.

**A greyed-out setting is one the renderer will not read** with the options you
have chosen, and that is worth more than it sounds. Highlight boost with motion
blur off, JPEG quality with PNG selected, ffmpeg arguments with a preset written
over them — each of those is unreachable in the renderer, and tuning one and
seeing no change is indistinguishable from the mod being broken. The relevance
rules are the same ones the in-editor Export screen uses.

Numbers are held to their accepted range when the file is written, and the tool
says which ones it moved. The renderer would clamp them silently on load, which
is how a `RenderSettleSubFrames=0` stays in a file being blamed for motion blur
that "does nothing".

Choosing a codec narrows the container and pixel-format lists to the ones that
actually work — a container that cannot hold the codec makes ffmpeg refuse, and
that failure is otherwise invisible. It writes line-by-line, so the comments in
the ini survive being edited through it.

The **Encoder presets** tab builds those files. Twenty-four codecs across the
same five groups, each with its own quality or profile ladder, encoding speed,
tune, pixel format and container — and an **Extra arguments** box for anything
not covered. A live preview shows the exact `Args=` and `Ext=` that will be
written.

Two things there are worth knowing. The codec list is filtered by what your
ffmpeg build actually contains, and **Test preset** encodes one frame with the
exact settings on screen and reports what ffmpeg said. That catches the case the
filtering cannot: every QSV and AMF encoder is present in the bundled ffmpeg and
none of them open without the matching hardware, so a preset built on one
produces no video and a stderr line you never see.

Where a profile decides the pixel format — ProRes and DNxHR both — they are one
choice rather than two, because neither encoder reconciles a mismatched pair;
they refuse at startup.

Paths resolve from the `.asi`, not from the game executable — under FiveM that
puts everything in `plugins\RockstarEditorPlus\`. Set `RenderOutputFolder` to an
absolute path to write renders off the game drive.

Per-marker settings are stored outside the `.clip` file, so projects stay
loadable without this mod installed. See [Per-marker settings](#per-marker-settings)
for how they are scoped.

An ini written by an older build will not contain keys added since. Those fall
back to their defaults, so nothing breaks — but a key has to be present before
you can change it. Delete the ini to have a current one written.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| Export gives a normal watermarked video | Capture add-on not found — the mod stepped aside rather than produce the wrong thing. Either ReShade was installed without add-on support, or `IgcsConnector.addon64` is not beside the executable. The log names which |
| Flags and ropes whip far too fast in the render | Sliding renders cloth once per sample instead of once per frame, so 64 samples means roughly 64× the speed. Engine limitation, no setting fixes it — see [Cloth and other live physics](#cloth-and-other-live-physics) |
| Flags and ropes completely still in the render | Walking pauses the replay, and live cloth never integrates while it is paused. The other half of the same limitation |
| Colours wrong in the render, fine on screen | Fixed in this build, with nothing to set. The add-on used to ask ReShade for the finished frame, and ReShade hands the channels back in a different order depending on its own version, so on some installs red and blue arrived swapped. It copies the back buffer itself now and reads the order from the buffer's own description. `RenderChannelOrder` and the **Colour Channels** row are gone |
| Multi-clip render stops after the first clip | The project's clip table read as empty, so multi-clip stepping never armed and the render ended where clip one did. It happens when the project was not fully loaded for playback — open it in the editor first, then Export. The log says so outright, with the clip count it actually read |
| A render ended early for no obvious reason | Every finish now logs the replay mode it stopped in. `EDIT` is normal; `LOADCLIP` means it stopped during a clip transition, which is worth reporting with the log |
| The video file will not open | If the log says `TERMINATED`, ffmpeg was killed before it finished and an mp4 or mov has no index without its final write — the footage is on disk but unreachable. The line after it says what ffmpeg was complaining about. Rendering to **MKV** survives this: it writes as it goes and loses only the last moment |
| Got frames, expected video | `RenderMode=Frames`, or ffmpeg not found — the log says which |
| A shake setting does nothing | Per-marker values override the ini and always win. If the menu shows a number rather than `Default`, that marker has its own. `ShakeDebugLog=1` logs what reached the camera |
| Shake looks frozen | The playhead is paused. Play or scrub |
| Nothing happens at all | Check the log. No file = the ASI never loaded; a log that stops early names the reason on its last line |

---

## Building

CMake, MSVC v143, C++20, x64. Static CRT — required, not preference: an ASI does
not choose which runtime its host already loaded, and MSVC's STL demands a
runtime at least as new as the compiling toolset. FiveM ships an older
`msvcp140` than current, which produced a crash on the first `std::mutex` lock
until the CRT was linked statically.

```
cmake -S . -B build.vs
cmake --build build.vs --config Release
```

Output in `build.vs/BIN/Release/`. The `.ini` files are generated from
`CMakeLists.txt` at configure time and copied next to the `.asi` — they are build
outputs and are not tracked.

---

## Licence

GPL-3.0-only. See [LICENSE](LICENSE).

Bundles [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause) and a
GPLv3 build of [FFmpeg](https://ffmpeg.org).

`IgcsConnector.addon64` and `IgcsDof.fx` are our modified builds of
[IgcsConnector](https://github.com/FransBouma/IgcsConnector) by Frans Bouma
(add-on MIT, `IgcsDof.fx` BSD 2-clause). Our fork lives at
[crxhvrd/simplecamera](https://github.com/crxhvrd/simplecamera) — it exists to
serve these mods and is not a drop-in replacement for the upstream add-on.
Licence texts and source information ship in `RockstarEditorPlus\`.
