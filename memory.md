# PxrMaskProjection — Dev Notes

End-user documentation lives in [README.md](README.md). This file is a
running log of what worked, what didn't, and the API / pipeline
gotchas uncovered while building the plugin.

---

## Status

**Working.** Renders correctly in Solaris on Windows 11 and has been
smoke-tested on Linux. Known-good combos:

- RenderManProServer **27.2**
- Houdini **21.0.559** (primary) and **20.5.613**
- MSVC **v143 / 14.44** on Windows 11
- g++ (any recent C++17) on Linux

---

## Repository layout

```
PxrMaskProjection/
├── README.md
├── memory.md                    ← you are here
├── .gitignore                   # excludes thirdparty/* and build artifacts
├── PxrMaskProjection.cpp        # Plugin source
├── PxrMaskProjection.args       # Sdr / USD UI registration
├── set_projection.py            # Houdini Python LOP helper
├── build.bat                    # Windows build (MSVC)
├── build.sh                     # Linux build (g++)
├── fetch_thirdparty.bat         # Windows dep fetcher
├── fetch_thirdparty.sh          # Linux/macOS dep fetcher
├── thirdparty/                  # populated by fetch_thirdparty (git-ignored)
│   ├── .gitkeep                 # keeps the dir tracked
│   ├── tinyexr.h
│   ├── exr_reader.hh
│   ├── streamreader.hh
│   ├── miniz.h
│   └── miniz.c
└── examples/
    └── test_mask.exr
```

### Why thirdparty/ isn't vendored

`thirdparty/*` is in `.gitignore`. The fetch scripts download
pinned versions:

- **tinyexr** — branch `release`, raw files pulled individually
  from `raw.githubusercontent.com/syoyo/tinyexr/release/`.
- **miniz** — GitHub release tag `3.0.2` (== version 11.0.2). The
  tag itself ships the *split* source (miniz.h pulls in 5 sibling
  headers); the amalgamated single-header form lives only in the
  release zip. We download the zip, extract just `miniz.h` and
  `miniz.c`.

Platform extraction tools:
- `fetch_thirdparty.sh` uses `curl` + `unzip` (BSD/GNU unzip both
  work).
- `fetch_thirdparty.bat` uses `curl` + the Windows-native bsdtar
  at `%SystemRoot%\System32\tar.exe` (shipping with Win10 1803+).
  Using the full path avoids Git Bash's GNU tar intercepting on
  `PATH`, since GNU tar can't read zips.

To update, bump `TINYEXR_REF` / `MINIZ_VER` at the top of each
script and re-run.

---

## Install targets

| File                  | Destination                                |
|-----------------------|--------------------------------------------|
| `PxrMaskProjection.dll` / `.so` | `$RMANTREE/lib/plugins/`          |
| `PxrMaskProjection.args`        | `$RMANTREE/lib/plugins/Args/`     |

---

## Why tinyexr (and not Houdini's / system OpenEXR)

Original Linux build linked against Houdini's `libOpenEXR.so` via
`$HFS/dsolib`. That worked on Linux but **crashed at `readPixels()`
on Windows** when loading against Houdini's `OpenEXR_sidefx.dll` —
ABI drift between the OpenEXR version the plugin was compiled
against (RenderMan's) and the one loaded at runtime (Houdini's).

Switching to [tinyexr](https://github.com/syoyo/tinyexr) (single
header, BSD) removed the dependency entirely. The plugin pulls in:

```
thirdparty/tinyexr.h          release branch
thirdparty/exr_reader.hh      pulled from release branch
thirdparty/streamreader.hh    pulled from release branch
thirdparty/miniz.h            standalone miniz (tinyexr no longer bundles it inline)
thirdparty/miniz.c
```

These are **fetched at setup time** by `fetch_thirdparty.*`; see
the repo-layout section above.

`miniz.c` gets compiled as a second translation unit alongside
`PxrMaskProjection.cpp`. `tinyexr.h` is included exactly once in
`PxrMaskProjection.cpp` via:

```cpp
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
```

Build scripts add `-I thirdparty` so `<miniz.h>` resolves.

---

## RenderMan 27 API — Gotchas

### 1. Factory + instance, not one class

RenderMan 27 splits projections into `RixProjectionFactory` (factory,
owns param table + create/destroy) and `RixProjection` (per-render
instance, owns `RenderBegin` / `Project` / `RenderEnd`).

```cpp
RIX_PROJECTIONFACTORYCREATE  { return new PxrMaskProjectionFactory(); }
RIX_PROJECTIONFACTORYDESTROY { delete static_cast<...>(factory); }
```

### 2. `RenderBegin` lives on the **instance**, not the factory

Params + `RixProjectionEnvironment` arrive in `RixProjection::RenderBegin`.
The factory's `CreateProjection` just news up the instance.

### 3. Method signatures (from the actual headers)

```cpp
// Factory
int Init(RixContext&, RtUString const pluginPath);
RixProjection* CreateProjection(RixContext&, RtUString const handle,
                                RixParameterList const*);
void DestroyProjection(RixProjection const* projection);

// Projection
RixSCDetail GetProperty(ProjectionProperty, void const** result) const;
void RenderBegin(RixContext&, RixProjectionEnvironment const&,
                 RixParameterList const*);
void Project(RixProjectionContext&);
void RenderEnd(RixContext&);
```

### 4. `RtUString` for every name

```cpp
RixSCParamInfo(RtUString("maskFile"), k_RixSCString, k_RixSCInput)
```

### 5. Resolve params by name, not enum index

hdPrman assigns its own param IDs; they do **not** match the
order in `GetParamTable`. Using a hard-coded enum like `k_maskFit=2`
silently reads the wrong param. Do:

```cpp
int pid = -1;
parms->GetParamId(RtUString("maskFit"), &pid);
parms->EvalParam(pid, 0, &m_maskFit);
```

### 6. `EvalParam` writes directly

```cpp
float f; parms->EvalParam(pid, 0, &f);   // RM 27
// NOT the old pointer-to-pointer pattern.
```

### 7. Base destructors are `protected`

Delete through `static_cast<DerivedType const*>(...)`.

### 8. `RixProjection` is a full camera replacement

You own ray generation — origin **and** direction. Killing rays
you don't want (direction = (0,0,0)) is secondary. Camera space
is +Z forward, origin at (0,0,0). See [RixProjection.h].

---

## Solaris / hdPrman integration

### Where projections live

- On the **Render Settings** prim, not the Camera prim:
  `ri:projection:name = "PxrMaskProjection"`.
- Params are namespaced: `ri:projection:PxrMaskProjection:<param>`.

### Setting it programmatically

`set_projection.py` uses `hou.LopNode.editableStage()` to stamp the
attributes onto the Render Settings prim. Drop-in as a Python LOP,
or point `LOP_PATH` at a stage-owning LOP and run from the Source
Editor.

### Args file tip

Changing `.args` sometimes requires restarting Houdini, since Sdr
caches node definitions. `RFH_ARGS2HDA=1` in `houdini.env` forces
a rescan.

---

## Ray generation

```cpp
// fov is an input param; focalScale is derived once in RenderBegin.
float half_narrow = min(half_sw_width, half_sw_height);
focalScale = half_narrow / tan(fov/2);

// Per ray in Project():
direction = normalize(sx, sy, focalScale);
```

### Why `fov` is a required parameter

At one point this was coded as `direction = normalize(sx, sy, 1.0)`
on the theory that hdPrman's `screenWindow` already encodes
`swT = tan(fov/2)`, which would make `dz=1` always correct.

**Tested empirically. It doesn't work.** With `dz=1.0` the
`impliedFOV` log line reports a value unrelated to the Houdini
camera (typically ~90–105° regardless of the scene camera), the
ground plane clips badly on tilted cameras (cam1), and the masked
region drifts as the camera moves.

The practical takeaway is that `screen[]` samples reaching the
projection don't carry the camera FOV in a form you can use without
knowing the FOV independently. So `fov` is kept as an explicit
parameter, matching `PxrPerspective`'s convention (narrower-dim
FOV in degrees).

### Auto-deriving `fov` from the render camera (Python side)

The plugin itself has no access to the USD stage — `RixProjection`
runs inside the render process with only the `RixProjectionEnvironment`
(width/height/screenWindow/clip/worldToCam) plus the plugin's own
param list. But `set_projection.py` runs in Houdini with full
stage access, so it:

1. Resolves the render camera via
   `UsdRenderSettings(rs_prim).GetCameraRel().GetTargets()[0]`.
2. Reads `focalLength`, `horizontalAperture`, `verticalAperture`
   off that `UsdGeomCamera` prim.
3. Computes
   `fov = 2 · atan( min(hAp, vAp) / 2 / focal )` in degrees.
4. Writes the result to `ri:projection:PxrMaskProjection:fov`.

Sanity check: Houdini's default aperture is 20.955 × 15.2908 mm
with focal 50 mm → `fov = 2·atan(15.2908/2 / 50) = 17.38°`, which
matches the empirical value the tester found by eye (17.5°). Any
standard UsdGeomCamera should round-trip through this formula.

`FOV_OVERRIDE` + `CAMERA_PRIM` escape hatches are there for the
"my camera aperture doesn't match the real sensor" or "I want to
mask against a different camera" cases.

### Kill, don't tint

Earlier versions also had a "partial" path — if `maskVal` was
between 0 and 1 (soft edge / feather), the plugin multiplied
`pCtx.tint[i]` by `maskVal`. Removed: by the time `tint` is
applied the integrator has already shaded the sample, so tinting
just darkens fully-paid pixels — it saves no render time, which
is the whole reason this plugin exists.

Current behaviour is strictly binary: `maskVal <= 0` → kill the ray
(`direction = 0`). `> 0` → keep at full strength. `threshold` +
`softEdge` reposition the cutoff but the resulting edge is sharp.

---

## UV mapping — fixes

### Y flip

EXR row 0 is the top of the image; `sy = +1` in screen space is the
top of the frame, but without a flip we were mapping it to EXR row
N-1 (= bottom of image). Result: rendered mask was upside-down.

```cpp
v = 1.0f - v;   // flip after nx/ny → uv
```

### Screen-window normalization

Early code divided by a renderAspect constant, assuming
`screen[] ∈ [-aspect, aspect] × [-1, 1]`. hdPrman's actual screen
window for a 16:9 render was `[-1.297, 1.297] × [-0.73, 0.73]` —
different proportions. Fix: normalize against the actual window
bounds from `RixProjectionEnvironment`:

```cpp
nx = 2 * (sx - m_swL) / (m_swR - m_swL) - 1;
ny = 2 * (sy - m_swB) / (m_swT - m_swB) - 1;
```

---

## Debug logging

`debug=1` (or `DEBUG=1` in `set_projection.py`) enables all
`msgs->Info/Warning/Error` calls — gated individually via
`if (m_debug && msgs) ...`. With `debug=0` the plugin is silent,
even on error.

Key lines to look for when troubleshooting:

- `RenderBegin env WxH screenWindow=[...] x [...] clip=[...]`
- `params maskFile='...' ... fov=... focalScale=...`
- `mask stats min=... max=... nonZero=.../...`
- `Project call numRays=... screen[0]=(sx,sy)` (first N buckets)
- `actual screen sample range x=[..] y=[..] impliedFOV=...x... deg`

The last one is the smoking gun for FOV misconfiguration — compare
`impliedFOV` against the Houdini camera's actual narrower-dim FOV.

---

## Windows build gotchas

VS 2026 Community's `vswhere` registration is missing, so
`VsDevCmd.bat` silently fails to set up the environment. `build.bat`
hard-codes paths:

```bat
set "MSVC=...\MSVC\14.44.35207"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "WINSDK_VER=10.0.19041.0"
```

Update these to your install's paths if they differ.

Also: Houdini **locks the plugin DLL** while the hdPrman delegate
is loaded. To redeploy a new build, close any `karma` / render
windows — sometimes the whole of Houdini — otherwise `copy` fails
with "being used by another process".

---

## History (chronological)

1. Started as a procedural circle/ellipse mask, no EXR.
2. Added EXR channel loading via Houdini's OpenEXR — compiled on
   Linux.
3. Plugin hooked up to Solaris via `ri:projection:name` on the
   Render Settings prim (not the camera — classic first miss).
4. RenderMan 27 API mismatch: old single-class `RixProjection`
   pattern didn't compile. Rewrote to factory + instance.
5. Hit `RtUString` / `EvalParam` signature changes — fixed.
6. Ported to Windows — EXR `readPixels()` crashed at runtime.
   Diagnosed as ABI drift between RMan-compiled OpenEXR and
   Houdini's `OpenEXR_sidefx.dll`.
7. Swapped Houdini OpenEXR for tinyexr (self-contained). Added
   `miniz` standalone since tinyexr's release branch no longer
   bundles it inline.
8. First real render: empty frame. Root cause: `RixProjection` is
   a full camera plugin — had to generate ray origin + direction,
   not just mask them.
9. `ri:projection:params` vs `ri:projection:PxrMaskProjection:...`:
   the latter is the correct namespacing for plugin params.
10. Next render: mask upside-down. Added `v = 1 - v` to map
    screen-top (`ny = +1`) to EXR row 0.
11. Render was horizontally narrower than the mask implied.
    `screen[]` range didn't match the assumed `[-aspect, aspect]`;
    switched to normalizing against the actual `env.screenWindow`.
12. Render scaled with camera distance (classic FOV mismatch).
    Instrumented Project() to print real screen sample range and
    implied FOV — confirmed 105° vs Houdini's ~26°. Added `fov`
    param, derived `focalScale = half_sw / tan(fov/2)`, and used
    it as `dz` in `normalize(sx, sy, dz)`. FOV=50 matched the
    tester's camera and rendered correctly.
13. Briefly theorised that `dz=1.0` should work unconditionally
    (screenWindow already encodes `tan(fov/2)`) and removed the
    `fov` parameter. Re-tested: broken — tilted camera clipped,
    implied FOV was wrong again. Rolled back; `fov` stays as a
    required user-supplied parameter.
14. Removed the "partial mask tints the beauty" path. Tinting
    happens after shading, so it can't save render time, which is
    the whole point of this plugin. Mask is now strictly binary:
    `maskVal <= 0` kills the ray, `> 0` keeps it at full strength.
15. Bug report: the masked render's composition didn't match a
    bypassed render of the same camera — mask shape was correct
    but the geometry inside appeared zoomed-out. Two causes:
    (a) `fov` parameter was being interpreted as "50 mm focal
        length" by the tester, but the parameter is FOV in
        degrees. README now calls this out explicitly.
    (b) Killed rays left the output pixel at hdPrman's default
        background color (flat gray), not black. `direction=0`
        tells the integrator to SKIP the ray — it never writes
        to the output pixel at all, so `tint=0` alone has no
        effect (it's never applied). Fix: give the ray a valid
        non-zero direction but `maxDist=0` — the integrator
        walks the "miss" path and writes the pixel, which we
        then force to black via `tint=0`. A few cycles per
        killed ray, far cheaper than full shading.
16. hdPrman's `screenWindow` is NOT plain `tan(fov/2)`. On a
    verified 17.5°-vertical camera we saw `screenWindow.top=0.730`
    — that would imply 72° under the standard convention. The
    scale factor (~4.74× for this camera) turns out to equal the
    `focalScale` the plugin derives from the user's `fov` input,
    i.e. `half_sw = focalScale · tan(fov/2)`. What that factor
    actually is (aperture-to-near-clip ratio? some Houdini-
    specific unit conversion?) remains unknown — and the
    `RixProjectionEnvironment` we get doesn't expose enough to
    back it out. So the plugin can't auto-derive `fov` from
    `screenWindow` alone.
17. Moved FOV auto-derivation into `set_projection.py`. The
    plugin side can't read USD, but the Houdini-side Python
    script can — so the script resolves the render camera via
    `UsdRenderSettings.GetCameraRel()`, reads
    `focalLength` + `horizontalAperture`/`verticalAperture`, and
    computes the narrower-dim FOV exactly the way
    `PxrPerspective` expects. Verified: Houdini default aperture
    20.955 × 15.2908 mm + focal 50 mm → 17.38°, matching the
    tester's empirical 17.5°. `FOV_OVERRIDE` and `CAMERA_PRIM`
    are kept as escape hatches but the common case is zero-config.
18. Cleaned up file layout and wrote `README.md` for public
    release.
19. Un-vendored the thirdparty single-headers — added
    `fetch_thirdparty.sh` / `.bat` so a clone stays tiny and
    license-clean, with the caveat that miniz's amalgamated form
    only exists in the GitHub release zip (the tag has split
    source).

---

## Potentially useful references

- [Writing Projections — RenderMan docs](https://rmanwiki-26.pixar.com/space/REN26/19661213/Writing+Projections)
- [RFH27 Cameras in Solaris](https://renderman.atlassian.net/wiki/spaces/RFH27/pages/544641152/Cameras+in+Solaris)
- [RFH27 Render Settings](https://renderman.atlassian.net/wiki/spaces/RFH27/pages/544641753/Solaris+Render+Settings)
- `$RMANTREE/include/RixProjection.h` — ground truth for the API.
- `$RMANTREE/include/RixIntegrator.h` — `RtRayGeometry` fields.
- `$RMANTREE/lib/plugins/Args/PxrPerspective.args` — reference
  projection (inspected during the FOV investigation).
- [tinyexr GitHub](https://github.com/syoyo/tinyexr)
- [miniz GitHub](https://github.com/richgel999/miniz)
