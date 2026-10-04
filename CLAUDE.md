# honeydew

Oscillating and clock reactions in a dish on a lightbox, for Resolume
Arena/Avenue, as two FFGL plugins from one core: `SW Honeydew` (`HD01`, source:
the dish on its lightbox) and `SW Honeydew Over` (`HD02`, effect: the clip is
the lightbox, and its light acts on the photosensitive chemistries). C++/GLSL,
CMake MODULE → two universal `.bundle`s (macOS) + Windows `.dll`s. MIT. Bundle
ids `com.stoatworks.ffgl.honeydew` and `com.stoatworks.ffgl.honeydew.over`.

Read `AGENTS.md` before touching a mechanism, a spectrum, the parameter
tables or the sync logic; `docs/CHEMISTRY.md` is where every number comes from.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- Render the source: `./build/hdtest --out /tmp/dish.png --frames 600`
- Render the Over effect on the harness's card: `./build/hdtest --over --out /tmp/o.png`
- List parameters: `./build/hdtest --list` (`--over` for the effect's)
- Set anything by name: `./build/hdtest --set "Reaction=5" --set "Depth=0.7"` (options by element index); `--set "Break Wave=1@100"` presses it at frame 100
- A beat into the Audio buffer: `--beat`; a raster: `--size WxH`; the clock's rate: `--fps`

## Verify
- Everything: `tools/verify.sh` (~15 min: reserved words, glslc, the pin, a fresh
  universal build, both bundles through lipo/plist/codesign/oxbow probe+selftest,
  every check at 1280x720 and 320x180, the cheap checks on Apple's software
  renderer at 320x180, the offline set, the negative controls, the mutants, the
  sweep, the bench)
- **The light**: `--spectra` (no GL), `--beer`, `--over-check`.
- **BZ**: `--oregonator` (period and duty against the three-variable Oregonator
  in double), `--fieldnoyes` (plane-wave speed against the 1-D double solution
  at three acids; Field & Noyes reported), `--spiral` (one +1/−1 pair), `--photo`.
- **The clocks**: `--clock` (the snap against the Harcourt–Esson closed form, Batch
  and Flow), `--sync` (Bar sync at 120 and 97 BPM: the clock, the blue bottle,
  the chameleon).
- **The rest**: `--briggs`, `--traffic`, `--bluebottle` (and the valentine),
  `--chameleon`, `--turing`, `--stir`, `--units`.
- **The machinery**: `--timebase` (Resolume's 499 million ms, the substep cap),
  `--resize`, `--prime`, `--state`; no GL: `--names`, `--transport`, `--timebase-law`.
- One raster only: add `--size WxH`. The software renderer:
  `HDTEST_RENDERER=software ./build/hdtest --beer --size 320x180`.
- **The checks can fail**: `--negative` (every wrong model; `--only NAME` for
  one), `tools/mutate.sh` (one character of GLSL and C++).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (`--no-context` empties the context table).
- Cost: `--bench` (720p/1080p/4K, both plugins, BZ at Detail 256 and 1024, BR, the blue bottle).

## Notes
- **The state is two RGBA32F textures per cell** (A and B, eight channels), whose
  meaning is the reaction's (`Chemistry.h`, `FreshState`); every chemistry pass
  reads them with `texelFetch`; only the stir pass reads between texels.
- **The grid follows Detail and the raster's ASPECT, not its size**; a resize
  of the same shape keeps the state to the bit, a new shape resamples it.
- **The per-texel step is ROS2** (the harness's own scheme, L-stable, second
  order) for BZ and CDIMA; the clock, the dye family and the chameleon use exact
  sub-steps (titration, exponentials, the first-order chain). The substep is
  planned from the stability bounds and capped at 96 a frame; past the cap the
  chemistry runs slow and `Diag` logs it (`--timebase` counts it).
- **Briggs–Rauscher runs on the CPU in double** (`BrEngine`), ten species on a
  32-column grid, on worker threads: it is stirred-only in 0.1.0.
- **Colour is Beer–Lambert** through 41 wavelength samples of each species'
  fitted spectrum (`Spectra.cpp`) against the CIE 1931 2° observer
  (`CieTables.h`, baked by `tools/bake_cie.py`); the Over's filter matrix is
  `I + Σ W_k (T_k − 1)`, exactly the identity for an empty dish.
- **Clock Sync** holds a quarter period, then sizes the clock's thiosulfate
  from the closed form (Batch or Flow), sizes a dye shake by bisection over the
  well-mixed model, and times the chameleon's dose for its manganate peak.
- Every host parameter is 0..1 except Seed (a real integer). An option reads
  back 0..1 whatever its count: map by element index. Names are unique after
  lower-casing and removing spaces (Arena's OSC address), and none has a `/`.
- No `sin`/`cos`/`atan` in the chemistry GLSL (the advect pass's rotation is
  the one exception, bounded); randomness is PCG integer hashing.
- GLSL reserved words must not be identifiers (`verify.sh` greps the 4.10
  list and the `noise1..4` built-ins). No `M_PI`, no `far`/`near`.
- `honeydew_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Provisional `StoatworksAbout.h` (`guide = ""`) and `ATTRIBUTIONS.md`: hand
  copies until the backend's sync scripts take over.

## Not done yet
- Never loaded into Resolume (oxbow probe and selftest only). No Windows build
  has run. No OpenFX port, no browser demo, no user guide.
- **Convection is not built**: the control is declared by neither plugin
  (AGENTS.md). Chemoconvection's `--rayleigh` and `--chemoconvection` do not exist.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile, the GL
vendor/renderer, and when the substep cap bites.

    ~/Library/Logs/honeydew/honeydew.YYYY-MM-DD.log
