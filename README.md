# PxrMaskProjection

A workround for non-rectangular region rendering

Tested with:

- Pixar RenderMan **27.2**
- Houdini **21.0.559**
- Windows 11 (MSVC v143) and Linux (g++)

## How it works

`PxrMaskProjection` is a full camera-replacement projection plugin
(`RixProjection`). For every screen sample hdPrman hands us, the
plugin:

1. Generates a perspective ray (`direction = normalize((sx, sy, 1.0))`).
   The RenderMan screen-window coordinates are already in camera-space
   units at the Z=1 focal plane, so no FOV parameter is needed.
2. Looks up the corresponding UV in the mask EXR (bilinear).
3. If the mask value is ≤ 0, zeros the ray direction so the
   integrator skips it.
4. If the mask value is between 0 and 1, multiplies the
   beauty-channel `tint` by that factor — useful for soft edges
   and partial transparency.

The EXR loader uses the bundled [tinyexr](https://github.com/syoyo/tinyexr)
single-header library, so the plugin does **not** link against
Houdini's or the system's OpenEXR — that avoids ABI-drift crashes
seen when loading against the DCC's own `OpenEXR_sidefx.dll` /
`libOpenEXR.so`.

## Features

- EXR mask loading by channel name (R, G, B, A, or custom)
- `maskFit`: Stretch / Fill / Fit
- `centerX`, `centerY`: pan the mask in screen space
- `invert`: swap black and white
- `threshold` + `softEdge`: hard cutoff with optional feather
- `debug`: verbose diagnostics in the render log

## Repository layout

```
PxrMaskProjection/
├── README.md
├── memory.md                    # Development notes / API gotchas
├── PxrMaskProjection.cpp        # Plugin source
├── PxrMaskProjection.args       # Sdr / USD UI registration
├── set_projection.py            # Houdini Python LOP helper
├── build.bat                    # Windows build (MSVC)
├── build.sh                     # Linux build (g++)
├── fetch_thirdparty.bat         # Windows dep fetcher
├── fetch_thirdparty.sh          # Linux/macOS dep fetcher
├── thirdparty/                  # Populated by fetch_thirdparty (not in git)
│   ├── tinyexr.h                #   ← BSD 3-Clause
│   ├── exr_reader.hh
│   ├── streamreader.hh
│   ├── miniz.h                  #   ← MIT
│   └── miniz.c
└── examples/
    └── test_mask.exr            # Sample mask image
```

Third-party single-header libraries (`tinyexr`, `miniz`) are **not
vendored in the repo** — run `fetch_thirdparty.*` once after
cloning to download pinned versions into `thirdparty/`. The refs
are set at the top of the fetch scripts; bump them and re-run to
update.

## Fetch dependencies

Before the first build, download the single-header deps:

```bash
# Linux / macOS — needs curl + unzip
./fetch_thirdparty.sh
```

```bat
REM Windows — needs curl + tar (both ship with Windows 10+)
fetch_thirdparty.bat
```

This writes `thirdparty/tinyexr.h`, `exr_reader.hh`,
`streamreader.hh`, `miniz.h`, and `miniz.c`. Re-run after bumping
the pinned refs (`TINYEXR_REF`, `MINIZ_VER`) at the top of either
script.

## Build — Windows

1. Run `fetch_thirdparty.bat` (see above).
2. Install Visual Studio 2022+ with the "Desktop development with
   C++" workload (MSVC v143 / toolset 14.4x).
3. Install RenderManProServer 27.2.
4. Edit the four path variables at the top of `build.bat` to match
   your system:

   ```bat
   set "RMANTREE=D:\Softwares\Pixar\RenderManProServer-27.2"
   set "MSVC=D:\...\VC\Tools\MSVC\14.44.35207"
   set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
   set "WINSDK_VER=10.0.19041.0"
   ```

   `HFS` is not required to compile — tinyexr removes the OpenEXR
   dependency — but leave the variable in place if you want to
   experiment with the Houdini toolkit.

5. From a normal `cmd.exe` or Explorer double-click:

   ```
   build.bat
   ```

Output: `PxrMaskProjection.dll` in the repo root.

## Build — Linux

1. Run `./fetch_thirdparty.sh` (see above).
2. Install `g++` (or `clang++`), standard `libstdc++` with C++17.
3. Install RenderManProServer 27.2.
4. Point `RMANTREE` at the install and run `build.sh`:

   ```bash
   export RMANTREE=/opt/pixar/RenderManProServer-27.2
   ./build.sh
   ```

Output: `PxrMaskProjection.so` in the repo root.

## Install

Copy the built library and the args file into RenderMan's plugin
folders:

| File | Destination | What it does |
|------|-------------|--------------|
| `PxrMaskProjection.dll` / `.so` | `$RMANTREE/lib/plugins/`      | The plugin code that RenderMan loads at render time. |
| `PxrMaskProjection.args`        | `$RMANTREE/lib/plugins/Args/` | Sdr schema — registers the plugin in Solaris's **Projection dropdown** and exposes its parameters in the UI. Without this file the plugin won't show up in the menu. |

On Windows (adjust paths):

```bat
copy /Y PxrMaskProjection.dll  "%RMANTREE%\lib\plugins\"
copy /Y PxrMaskProjection.args "%RMANTREE%\lib\plugins\Args\"
```

On Linux:

```bash
cp PxrMaskProjection.so   "$RMANTREE/lib/plugins/"
cp PxrMaskProjection.args "$RMANTREE/lib/plugins/Args/"
```

Alternatively, keep the files in a custom folder and point
`RMAN_RIXPLUGINPATH` (for the `.so` / `.dll`) and
`RMAN_SHADERPATH` (for the `.args`) at it — see
[RenderMan's docs](https://rmanwiki-26.pixar.com/space/REN26/19661213/Install).

After deploying the files, **restart Houdini** — Sdr caches node
definitions on launch, so a running session won't pick up a new
`.args` file. If the entry still doesn't appear in the Projection
dropdown, see [Troubleshooting](#plugin-doesnt-appear-in-the-projection-dropdown).

> Note: you can also skip the dropdown entirely and set the
> projection from a Python LOP using `set_projection.py` (see
> below). The `.args` file is still required — hdPrman routes
> USD attributes through Sdr to reach the plugin.

## Usage in Solaris

The projection lives on the **Render Settings** prim, not on the
Camera prim. Easiest way to set it up is via `set_projection.py`:

1. In a Houdini LOP network, drop a `Python` LOP downstream of your
   Render Settings.
2. Paste the contents of `set_projection.py`.
3. Adjust the constants at the top:

   ```python
   MASK_FILE    = "D:/path/to/mask.exr"
   MASK_CHANNEL = "R"          # or A, G, B, custom
   MASK_FIT     = 0            # 0=stretch, 1=fill, 2=fit
   DEBUG        = 0            # 1 for verbose render-log output
   ```

4. Render with hdPrman.

Raw USD equivalent (what the script writes):

```usda
def Scope "Render"
{
    def RenderSettings "rendersettings"
    {
        string ri:projection:name = "PxrMaskProjection"
        string ri:projection:PxrMaskProjection:maskFile    = "..."
        string ri:projection:PxrMaskProjection:maskChannel = "R"
        int    ri:projection:PxrMaskProjection:maskFit     = 0
        float  ri:projection:PxrMaskProjection:centerX     = 0.0
        float  ri:projection:PxrMaskProjection:centerY     = 0.0
        int    ri:projection:PxrMaskProjection:invert      = 0
        float  ri:projection:PxrMaskProjection:threshold   = 0.0
        float  ri:projection:PxrMaskProjection:softEdge    = 0.0
        int    ri:projection:PxrMaskProjection:debug       = 0
    }
}
```

## Parameters

| Name | Type | Default | Description |
|------|------|---------|-------------|
| `maskFile`    | string | `""` | Path to EXR mask image. |
| `maskChannel` | string | `"A"` | Channel name inside the EXR. |
| `maskFit`     | int    | `0` | `0`=Stretch, `1`=Fill, `2`=Fit. |
| `centerX`     | float  | `0.0` | Horizontal mask offset, screen-space `[-1, 1]`. |
| `centerY`     | float  | `0.0` | Vertical mask offset, screen-space `[-1, 1]`. |
| `invert`      | int    | `0` | `1` to invert the mask. |
| `threshold`   | float  | `0.0` | Mask values ≤ threshold are killed. |
| `softEdge`    | float  | `0.0` | Feather width above the threshold. |
| `debug`       | int    | `0` | Verbose render-log output. |

## Troubleshooting

### Plugin doesn't appear in the Projection dropdown

- Check `PxrMaskProjection.args` was copied to
  `$RMANTREE/lib/plugins/Args/` (not just `plugins/`).
- Some Houdini versions cache Sdr nodes aggressively — restart
  Houdini after deploying a new `.args` file.
- If you rename or change schema attributes in `.args`, delete
  the Houdini Sdr cache or use `RFH_ARGS2HDA=1` in
  `houdini.env`.

### Crash on render / nothing renders

Enable `debug=1` and check the render log (`mplay`'s Render Log,
or the Houdini terminal). You should see:

```
PxrMaskProjection: RenderBegin env 1280x720 screenWindow=...
PxrMaskProjection: params maskFile='...' channel='...' fit=0 ...
PxrMaskProjection: loaded '...' channel '...' (WxH)
PxrMaskProjection: mask stats min=... max=... nonZero=.../...
PxrMaskProjection: Project call, numRays=... screen[0]=(sx,sy)
PxrMaskProjection: actual screen sample range ... impliedFOV=...x... deg
```

Common causes:

- **Empty render / wrong perspective** — check that `ri:projection:name`
  is actually being applied to the render settings prim, not the camera
  prim. Use `debug=1` and verify `RenderBegin` is being called.
- **Channel not found** — the log prints a list of available
  channels in the EXR. Pick one that exists.
- **Plugin loads but hangs** — make sure you built against the
  same RenderMan version you're rendering with.

### Windows-only: build.bat says "not recognized"

Some VS installs don't register `vswhere`, so `VsDevCmd.bat`
silently fails to set up the environment. `build.bat` sets the
needed paths by hand — just make sure the four `set` lines at the
top match your install, including the MSVC toolset version
number (e.g. `14.44.35207`) and Windows SDK version
(e.g. `10.0.19041.0`).

## License

Plugin source (`PxrMaskProjection.cpp`, `PxrMaskProjection.args`,
build scripts, `set_projection.py`) — MIT.

Third-party:

- `thirdparty/tinyexr.h`, `exr_reader.hh`, `streamreader.hh` —
  BSD 3-Clause (Syoyo Fujita / tinyexr contributors).
- `thirdparty/miniz.h`, `miniz.c` — MIT (Rich Geldreich /
  miniz contributors).

See each file's header for the full text.

RenderMan, hdPrman, and related trademarks are property of
Pixar. This project is not affiliated with or endorsed by Pixar.
