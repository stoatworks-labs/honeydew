# honeydew — for agents

The why behind the code. `CLAUDE.md` has the commands; `docs/CHEMISTRY.md` has
every constant with its source (or its stand-in status); this file has the
reasoning, the traps that were actually hit, and what is and is not known.
Built 2026-10-04 in one session (the Mac slept once, at about 07:14, and the
build resumed from the tree) from `~/Projects/resolume/specs/SPEC-honeydew.md`,
which changed in place four times while the build ran (the traffic light, the
rest of the blue-bottle family, chemoconvection, the chemical chameleon). The
briefs are `BRIEF.md` and `BRIEF-ADDENDUM.md` beside it.

## The one idea

**A thin layer of real reagents in a dish on a lightbox.** Every reaction is a
published mechanism with its rate constants in mol/L and seconds; the species
diffuse with diffusion coefficients in cm²/s across a dish whose width is in
millimetres; and the colour is never a palette. Each coloured species has a
molar absorption spectrum ε(λ) (a sum of Gaussians fitted to the cited peaks,
41 samples from 380 to 780 nm); the layer's transmittance is Beer–Lambert
through the Depth; the picture is the lightbox's spectrum through it,
integrated against the CIE 1931 2° observer and taken to sRGB. The red of
ferroin and the blue of ferriin, the amber of iodine, the blue-black of the
starch–iodine complex and the grey of permanganate-with-manganate all fall out
of concentrations, and the harness checks the pixels against a double-precision
CPU integral of the same chemistry.

The house-style claim is that the plugin IS the chemistry, so the harness asks
it for the published facts: the Oregonator's period, the trigger wave's speed
against the 1-D solution of the same PDE, the Harcourt–Esson clock's switch
time from its closed form, the Briggs–Rauscher period from the De
Kepper–Epstein mechanism in double, the Lengyel–Epstein wavelength, the blue
bottle's fade against the well-mixed model, and the chameleon's chain.

## How a frame goes

1. **Time** (`Honeydew.cpp`, `Clock.h`): the host's elapsed time, frame to
   frame, in double (the unit voted against the wall clock: conway's and
   rosette's code); a jump (backwards, or over 1 s) passes no time. Chemical
   time is host dt × Time-lapse (1–300×).
2. **Transport** (`Transport.h`): `SetBeatInfo( bpm, barPhase )` gives the
   seconds to the next beat or bar; with no host transport the plugin runs its
   own phase at the last BPM it saw (120 at first).
3. **Audio**: an onset (primed: the first frame after a trigger fires nothing)
   is a Drop (Audio Drops) or a Shake (Audio Shakes).
4. **Presses and the sync**: Reset reseeds; Drop, Break Wave and Shake are
   events; Clock Sync holds a quarter of the period after a transition, then
   sizes (the clock, the dye family) or times (the chameleon) the next dose so
   the transition lands on the next beat or bar.
5. **The grid**: columns from Detail (128/256/512/1024), rows from the raster's
   ASPECT, never its size. The cell is Dish Width / columns, in mm. A changed
   shape resamples the state before any pass binds.
6. **Seed**: both state textures are written from the reaction's `FreshState`
   with a 2% hash noise; BZ gets its pacemaker sites (one per 24×24-cell block
   with probability 0.3, radius 2.5 cells); the Over with Seed From Clip
   excites the bright parts of the clip.
7. **Steps**: `ChemicalClock::Plan` splits the frame's chemical time into
   substeps from the diffusion bound (dt ≤ 0.2 h²/D_max) and the reaction's
   own limit, capped at 96 a frame (the rest is dropped, never banked, and
   logged). Each substep is step A (the reaction, per texel: ROS2 for the
   Oregonator and Lengyel–Epstein, an exact titration for the clock, exact
   exponentials for the dye family and the chameleon's chain), then step B
   (five-point diffusion with no-flux edges inside the Petri mask, the Flow
   feed, the clip's light, the doses and drops). Stirred, an advect pass
   (semi-Lagrangian through a Rankine vortex) and a relax pass (towards the
   dish mean) run too. Briggs–Rauscher runs instead on the CPU in double
   (`BrEngine`): ten species, 32 columns, worker threads, uploaded as the
   state.
8. **Reductions**: a column pass and a total pass give the dish means the
   sync needs.
9. **Colour**: per pixel, the species' concentrations from the nearest cell
   (hand bilinear), the iodine speciation and the starch's Langmuir binding,
   the absorbance Σ εᵢ(λ)cᵢ, `T = exp( −ln10 · A · depth )` at 41 wavelengths,
   the lightbox's weights (source) or the primaries' weights (the Over's 3×3
   filter matrix `I + Σ Wₖ ( Tₖ − 1 )`, exactly the identity for an empty
   dish), Exposure, then sRGB.
10. **Composite**: the source is opaque (the lightbox is the picture); the
    Over is `clip + Mix · ( filtered − clip )` with the clip's alpha, a
    `texelFetch` of the clip so Mix 0 is bit-exact.

### The state

Two RGBA32F textures per cell, A and B, eight channels whose meaning is the
reaction's (`Chemistry.h`, `FreshState`): BZ holds the Oregonator's x, y, z,
the bromate and the malonic acid; the clock holds H₂O₂, I⁻, I₂ (+I₃⁻) and
thiosulfate; CDIMA holds u, v; the dye family holds the dye's forms, oxygen,
glucose and hydroxide; the chameleon holds MnO₄⁻, MnO₄²⁻, MnO₂ and glucose.
Everything chemical reads them with `texelFetch`; only the advect pass reads
between texels.

## Decisions taken without asking

- **Three-variable Oregonator, not Tyson–Fife.** At the recipe (0.3 M
  bromate, 0.3 M H⁺) q = 9.5e-5 against ε′ = 1.8e-4, so the bromide is not
  fast at rest and the two-variable reduction misses the period by 23% (77.6 s
  against 101.0 s). The plugin integrates x, y, z with ROS2 per texel;
  `--oregonator` reports the reduction's error.
- **f = 1.4 at the recipe**, so the dish oscillates in bulk and the pacemakers
  lead it (targets); the excitable checks (`--fieldnoyes`, `--spiral`,
  `--photo`) set f = 2.6, just past the Hopf point, through the test hook.
  k₅ = 1 M⁻¹ s⁻¹ (Tyson's order) is the one Oregonator STAND-IN.
- **Pacemakers are a closure.** A real dish's targets come from dust and
  scratches; here a hashed site per 24×24-cell block with probability 0.3,
  radius 2.5 cells, a raised bromate there. Seed changes them.
- **The pushed-front speed is reported, not gated, against Field & Noyes.**
  The plugin's 0.1017 mm/s at 1× is held to the 1-D double solution of the
  same PDE (0.1008); Field & Noyes 1974's law gives 0.1200 and the exponent
  on [H⁺] comes out 0.80 against their 0.5 — the Oregonator at these
  constants is a pushed front, not their pulled one. Both numbers are printed.
- **Briggs–Rauscher is stirred-only and runs on the CPU in double.** Ten
  species, rtol 1e-4, a 32-column grid with diffusion and relaxation, no
  vortex. A float GPU step of this stiffness was not going to hold 1%.
  Flow's residence is 800 s (1/156 and 1/400 quench the transcribed model).
- **CDIMA's malonic acid is 3.5 mM, not the table's 2.25 mM**: the table's
  value sits 10% outside the reduced model's Turing region. The Turing
  wavelength is 0.12 mm (the fastest-growing mode; the onset's k_c gives
  0.108), so the pattern wants a ~10 mm dish or Detail 1024.
- **The chameleon renders without hypomanganate** and the sequence is
  purple → grey → green → yellow-brown (see "The blue", below). Clock Sync
  TIMES its dose, because the manganate peak lands ln(β/α)/(β − α) after a
  dose whatever its size.
- **The dye family is one engine with the dye as data**: the oxidised form,
  the one-electron form (the traffic light's red; the valentine's resorufin
  for the irreversible step), the leuco form, oxygen, glucose, hydroxide.
  Pons 2000's rate law (k₁ 2000 M⁻¹ s⁻¹, k₂ from k_obs 0.0042 s⁻¹) is the
  mandated one; the indigo carmine and resazurin constants are STAND-INS by
  analogy, calibrated to demonstration timings. Indigo carmine's pK_a is
  taken as 12.6 so the 1× recipe (pH 12.9) reads green and ¼× reads blue.
- **Aeration is a closure.** A still layer takes oxygen through its top at
  k_La = 2 D_O₂ / d² (1 + 20 Stir²); a Shake is a burst decaying over 0.7 s
  of real time at 200× that, plus the stirring burst. The methylene blue
  dimer (K 2000 M⁻¹, a STAND-IN) falls out of a monomer–dimer equilibrium.
- **Stirring is a closure**: a Rankine vortex (ω = 4π·Stir rad/s), an eddy
  diffusivity 2·Stir² mm²/s, and above Stir 0.6 a relaxation of every cell
  towards the dish mean (3·((Stir − 0.6)/0.4)³ s⁻¹), so Stir 1 is a stirred
  beaker and 0.3 is a bar turning in a layer.
- **Flow is a CSTR** with a 300 s residence (a STAND-IN; BR's 800 s), the
  feed at the recipe; Batch runs down (BZ's malonic acid is spent over ~3600
  s at the recipe, the clock's and the chameleon's reagents by their doses,
  the dye family's glucose by its cycles).
- **Reactor defaults to Flow** so BZ never runs down on stage; Batch is the
  demonstration.
- **Convection is not built**, and the control is declared by neither plugin
  (below).
- **Recipe sliders are log ¼×–4× with 0 at the bottom meaning none**: an
  Indicator of 0 is an empty dish, which is what `--beer` uses to prove the
  Over's identity.
- **Dish Width 10–300 mm, Depth 0.3–20 mm, Time-lapse 1–300×, Drop Size 1–20
  mm, Auto Drop 0 / 1–60 a minute, Exposure ±2 stops**, all geometric; the
  defaults are 60 mm, 1.5 mm, 30×, 4 mm.
- **Drop Position** is Random, Centre and (the Over only, last) Brightest, so
  the shared options keep their indices.
- **The lightboxes**: D65 is the CIE table; LED 5000K and Warm White are a
  450 nm pump plus a phosphor band (modelled, CHEMISTRY.md). Through an empty
  dish D65 is sRGB white to 2e-3. **The Over has no Lightbox**: its light is
  the clip, filtered through the display primaries; the first sweep found the
  declared one dead (both ends identical) and it was withdrawn rather than
  given a meaning it does not have.
- **Clock Sync** holds a quarter of the period after a transition
  (`kSyncHold`) and leads the bar by 0.25 s (`kSyncLead`). The clock's dose
  is the closed form's, plus twice the iodine already present (it takes two
  thiosulfate each) and with the iodide the titration gives back; in Flow the
  H₂O₂ relaxes to the feed and the switch time is found by bisection. The dye
  family's shake is sized by bisection over the well-mixed model, retrying up
  to three boundaries if the fade cannot land on the next one; if none lands
  it gives a full shake and logs it.
- **The `--names` check covers the GLSL's #defines**: the 70 constants the
  shaders use for reactions, species and parameter slots are checked against
  the C++ enums, so a reordered enum cannot silently point a shader at the
  wrong slot.
- **Commit trailer** is the brief's `Claude Fable 5.1`.

## The blue (the chameleon's disputed colour)

The spec asked whether permanganate and manganate seen together read blue. Through
these spectra they do not: at 25% of the Mn as manganate (1.3 mM in 1.5 mm) the
mixture is hue 305, saturation 0.40 (purple); at 50% hue 278, saturation 0.19
(a grey); at 75% hue 163, saturation 0.25 (a grey-green). Blue would be ~240.
So the "blue" the demonstration shows needs hypomanganate (Mn V, near 670 nm),
whose kinetics and spectrum were not found to cite, and the plugin follows the
hypomanganate account by omission: purple → grey → green → yellow-brown.
`--spectra` prints the three mixtures every run.

## Convection is not built

The spec's third change asked for depth-resolved chemoconvection (Boussinesq
in the blue-bottle family, a Convection control, `--rayleigh` against
Chandrasekhar's rigid–free Ra_c 1100.65 and k_c·d 2.682, `--chemoconvection`
against Pons 2000/2008). It was triaged last and the 2-D set, the docs and
verify came first, as the spec said to do if the solver threatened the rest.
It is not in 0.1.0: no solver, no checks, and **the Convection control is
declared by neither plugin** (`Controls.cpp` says why in place), because a
control that does nothing fails the sweep and lies to the user. The aeration
closure above is what a still layer gets. Status says so plainly.

## The traps

**Tyson–Fife is not a reduction at this recipe.** The first BZ was the
two-variable model and missed the Oregonator's period by 23%, because q and ε′
are the same order (the bromide is not fast at rest at 0.3 M bromate). Three
variables, and the reduction's error is printed by `--oregonator` so nobody
puts it back.

**Implicit Euler per texel was 5% off the double reference**; ROS2 (one
`inverse( mat3 )` and two solves a step, L-stable, second order) holds it
within the Richardson bound. The bound is real: every reference is run at its
step and at half its step and the plugin at its substep and half (the test
hook `SetSubstepScaleForTest( 0.5 )`), and the bound is a frame plus twice
each difference.

**Halving the substep must actually halve it.** The first `SetSubstepScaleForTest`
scaled the diffusion bound only; a reaction-limited plan ran the same substeps
twice and the Richardson term read 0. It now scales whichever limit wins and
forces at least twice the substeps.

**A period read at the pixel minima jitters by a frame or two**; the period
is read at mid-level crossings now (and the Oregonator's at the red
channel's).

**Fixed-step ROS2 on Lengyel–Epstein blows up at small σ.** With no starch
(σ = 1) the model is a relaxation oscillator whose activator crashes within a
fraction of a t′, and the step that is fine at σ 151 (0.034 s) diverges at
0.0085 s; stable at 0.004 s, measured on the CPU at the 1× recipe (0.0085 at
σ 2.5, 0.017 at 5.5, 0.068 from 11.5 up). The reaction limit now scales with
σ/15 up to 1 (0.0034, 0.0057, 0.0126, 0.0264 s at those σ). A stiff solver's
stable step is a function of the recipe, not a constant: derive it from the
recipe or measure it across the control's range. **Fleet-wide.**

**A Turing pattern needs its e-foldings.** The fastest-growing mode grows at
0.0063 s⁻¹ (e-fold 159 s) at the recipe; 120 s showed nothing and the check
was failing honestly. It runs eight e-foldings now (1271 s) and holds the
wavelength to the fastest mode, not the onset's k_c (0.1205 against 0.1077
mm: one FFT bin apart on a 10 mm dish, so the distinction is real).

**"Stays uniform" is a time-average, not a snapshot.** Without starch the
cells' phases sit a little apart from the 2% seed noise and at a spike the
instantaneous contrast reaches 2; what a Turing pattern has and an
oscillation has not is a time-average with structure (0.69 against 0.0016
over 300 s). A no-pattern check that reads one frame fails on phase waves.
**Fleet-wide** for any oscillating medium.

**The rig's default Reactor (Flow) silently changed what three checks
measured.** Flow washes the clock's thiosulfate, the chameleon's permanganate
and the stir tracer out; `--clock` read 10% early, the chameleon's rings
never formed and the sync landed early. Every check now sets its reactor, and
the Flow cases have their own closed forms (the clock's with the H₂O₂
relaxing to the feed; the chameleon's peak with k₀ in it). A check must set
every control it depends on. **Fleet-wide.**

**The starch–iodine complex dims red, not blue.** Its band is at 620 nm, so
the "blue-black" snap is a fall in the RED channel; the clock's switch is
read there, and the BR's blue-black needed the Langmuir binding (the free
triiodide alone is amber) before it showed at 7 mm.

**The sync's clock dose was short by the iodine already there**, and by the
iodide the titration returns: the closed form assumes a fresh dish. Both are
in the dose now; the sync holds to a frame at 120 and 97 BPM.

**The dye sync got stuck when the next bar was too near**: no shake size
lands a fade in 0.3 s. It retries the next boundary (up to three) and, if
none lands, gives a full shake and logs it rather than freezing.

**A Shake's burst skews the timing** the fade checks read, so the checks dose
oxygen through `DoseForTest` (the shake without its stirring burst) and read
the fades from the state as well as the pixels.

**Literature dye strengths are opaque at 7 mm.** Methylene blue at Pons's
4.6e-5 M is fine; indigo carmine at 1e-3 M and resazurin at 1e-4 M were
black. The traffic light runs 2e-4 M and the valentine 3e-5 M, chosen to
absorb about one unit in 7 mm, and CHEMISTRY.md says so.

**The semiquinone at 0.3× the first step's rate made the red linger** for
most of the cycle (the demonstration's red is brief); it is 1.0× (a
STAND-IN either way).

**The traffic light's transitions are a slow hue drift across a class
boundary**, and the float colour sits ~1e-4 from the double's, so the
reference and the plugin can cross a boundary two frames apart with the same
chemistry. The bound is a frame, plus the Richardson term, plus 1% of the
time (687 against 683 s on a 7.3 s bound). A classifier's edge needs slack in
proportion to the slowness of the thing crossing it. **Fleet-wide.**

**The chameleon's rings needed the drop to be part-mixed.** With the glucose
flat, the drop's edge (thin in permanganate) reduced first as the spec says,
but with D(MnO₄⁻) 1.5e-5 cm²/s the rings never separated in a 4 mm drop; a
drop spreads a radial glucose profile as it lands (it thins the glucose where
it falls), and the rings come in the right order.

**The stir tracer must survive both reactors.** Thiosulfate is eaten at once;
iodine is washed out in Flow; the MnO₂ colloid (D 1e-7) is the tracer that
stays put until the stirring moves it.

**Briggs–Rauscher's Flow at the usual 1/156 s⁻¹ quenched the transcribed
model**, and so did 1/400; 800 s sustains it. The model's own period (248 s
in Batch, 140 in Flow) is what the plugin is held to, not the demonstration's
"every few seconds" — the De Kepper–Epstein constants at Shakhashiri's
proportions give minutes, and CHEMISTRY.md says which chain of custody the
constants have (Binous via Paredes).

**Wave speed depends on Detail**: at 0.2 mm cells the front is thinner than a
cell and runs 18% fast. `--units` gates speed within equal-cell groups across
Dish Width and raster, and holds the plugin's 0.2 mm / 0.1 mm ratio (1.176) to
the 1-D double line's own at the same cells (1.175, bound 0.05): the
discretisation's effect, not a units error. It prints the converged 1-D speed
(0.0874 mm/s at 0.025 mm) beside it. The first version only reported the
ratio, and its negative control (a cell fixed at 0.1 mm whatever Dish Width
says, which doubles the 0.2 mm group) PASSED — a check that gates only within
groups cannot see a wrong unit shared by a group.

**A wrong model shared by the reference and the plugin is not a negative
control.** The Oregonator's negative doubled ε in the model object the
reference integrated AND handed the plugin, so both moved together (period
106.6 against 106.6) and the check passed. The reference keeps the recipe;
only the plugin's ε is doubled (107.1 against 101.0, bound 1.75: caught).
Two of twenty negatives were found this way on the first full run. The
negative run is the only thing that finds this. **Fleet-wide.**

**Apple's software renderer gives two contexts different floats.** Two Over
rigs on the SAME card, Light Coupling 0, agree exactly on the GPU for 80 s of
a Ru-BZ wave and differ on the software renderer from frame 5 (5e-4 to 2e-3
by the end, growing as the excitable medium amplifies rounding). A bit-exact
cross-context comparison proves nothing there, so `--over-check` runs a
third rig as the control and prints a `skip` with the numbers where the
control differs; on the GPU the comparison stays exact (0 floats differ, the
control 0). Same-context comparisons (Mix 0 bit for bit) hold on both.
**Fleet-wide** for any check that compares two contexts' float state.

**The Over's Beer check sampled the card's dark band** on its first run (a
transmittance through black is black); it samples the plain grey region.

**`bindMRT`'s GLenum initialiser list narrowed** (`GL_COLOR_ATTACHMENT0 + i`
is an int) and MSVC would have refused it; `static_cast`. `Params` was used
before its typedef in `Chemistry.h` and clang said so in the second file to
include it, not the first.

**The Mac slept mid-build** (07:14); nothing was lost but a stale `build/`
timestamp. Commit as you go.

Carried from the fleet and respected here: `SetTextParameter` returns
`FF_SUCCESS` for About; an OBJECT library with file-scope registrations;
`SetBeatInfo` for the transport; an option reads back 0..1 whatever its count
(map by index); `FF_TYPE_INTEGER` for Seed; allocate before binding; scoped
bindings cleared to 0 and `unbindTextureUnits`; GL state captured and
restored including the colour mask and the draw buffer; names within 16 bytes
and unique as Arena addresses them, none with a `/`; no `M_PI`, `<cmath>`
included; `packed`, `smooth`, `sample`, `noise1..4` and the 4.10 reserved
list never identifiers; integer hashing (PCG) only; the clock-unit vote and
double time; primed onsets; `far`/`near` avoided.

## Would this hold on another rasteriser, at another raster?

Every check runs at 1280x720 and at 320x180 on this Mac's GPU, and the cheap
state-reading set at 320x180 on Apple's software renderer (`verify.sh`). CI
runs only `--offline` and glslc: a macOS runner has no accelerated GL. The
grid follows Detail and the aspect, never the raster, so a raster change
changes only the composite's sampling; every chemical number below is the
same at both rasters to the digit printed.

| check | bound | why that number | raster / rasteriser |
| --- | --- | --- | --- |
| `--spectra`, `--transport`, `--timebase-law`, `--names` | exact / 2% of a peak / 2e-3 of white | CPU, double; the 2% is the Gaussian sum's local-max tolerance within ±8 nm | CPU |
| `--beer` | 1e-6 of a pixel against the double integral; the empty dish within one 8-bit level (source) or float ULP (Over) | 41 samples in float against 41 in double: 1e-7 measured; `exp` and `dot` are the only operations | both rasters, both rasterisers |
| `--over-check` | Mix 0 bit for bit; Light Coupling 0: 0 state floats differ, with a same-clip control at 0 | `texelFetch` and `a + 0 · ( b − a ) = a`; the chemistry pass never reads the clip when the coupling is 0 | Mix 0 on both rasterisers; the cross-context comparison on the GPU only (a skip, with the numbers, on the software renderer: the traps) |
| `--oregonator` | a frame + 2 × the plugin's half-substep difference + 2 × the reference's (1.39 s on 101 s) | Richardson on both sides; measured 0.22 s off | raster-free (stirred) |
| `--fieldnoyes` | one cell over the run + the Richardson terms | the front's position is read to a cell | both rasters (the grid is the same) |
| `--spiral` | exactly one +1 and one −1 winding number in ≥ 95% of samples; 8 of 8 probes swept with a spread under 5% | integer winding numbers | both rasters |
| `--photo` | stopped / crossed, boolean, at full light and below the model's own 1-D threshold | the threshold (φ 0.0014–0.0019) comes from the 1-D double line | both rasters |
| `--clock` | a frame + the Richardson term (0.25–0.30 s) | the titration step is exact; only the frame and the H₂O₂ step remain | both rasters, both rasterisers |
| `--sync` | a frame + the hold's rounding (0.037 s; 0.050 for the fades) | the dose lands on a frame boundary | both rasters |
| `--briggs` | 1% of the period + a frame | the CPU engine in double at rtol 1e-4 against the same model at 1e-8 | raster-free (CPU) |
| `--traffic`, `--bluebottle` | a frame + Richardson + 1% of the time; hues to the class | the classifier's slack, above | both rasters |
| `--chameleon` | a frame + Richardson; ring order exact; Batch darkening monotone | exact exponentials | both rasters |
| `--turing` | one FFT bin of the fastest mode's wavenumber; contrast > 0.1; the time-average < 0.05 | a 10 mm dish gives 83 bins at 0.12 mm | both rasters |
| `--stir` | variance decays faster with Stir; in-phase within a frame | ordering, not a value | both rasters |
| `--units` | 0.00078 mm/s within an equal-cell group; the 0.2/0.1 mm ratio within 0.05 of the 1-D line's | the cell is the only length; a cell of front-finding over the interval | two rasters, two Dish Widths, three Details |
| `--timebase` | dt within 1e-9 s at 499,000,000 ms; the cap counted exactly | double time; integer substep counts | raster-free |
| `--resize`, `--prime`, `--state` | exact | the state copies, the counts, the GL state | `--resize` crosses 1280x720 ↔ 320x180 ↔ 960x720 |

**What would move on another GPU**: ROS2's `inverse( mat3 )` in float. The
Oregonator's Jacobian is well conditioned at these parameters (the bound is
met with 1.2 s to spare on 101 s), but a GPU with a different `exp` or a
fused-multiply-add policy will land the period a frame or two elsewhere
inside the same bound. The software renderer runs the clock (exact
exponentials) bit for bit against the GPU; it does not run the Oregonator in
verify (minutes), so that is assumed, not measured, there.

## A check that cannot fail is not a check

`hdtest --negative` sets one wrong MODEL at a time — a test hook in the
plugin where the model is the plugin's, so the shipped shader computes the
wrong thing — and requires the check to fail. Every physics check also
requires the measured thing to have happened (a wave reached the far third,
the snap occurred, the fade occurred, the rings formed), so a check cannot
pass on a dead dish. All 20 are detected at 320x180:

| check | the wrong model |
| --- | --- |
| `--spectra` | the reference observer shifted 40 nm |
| `--transport` | a beat counted as a bar |
| `--beer` | the plugin squares the half-depth colour per channel instead of integrating |
| `--over-check` | Light Coupling 0 still couples the clip |
| `--prime` | the analyser unprimed |
| `--resize` | the state cleared on a resize |
| `--timebase` | elapsed time from the host clock in float |
| `--oregonator` | the plugin's ε doubled against the reference |
| `--fieldnoyes` | diffusion switched off (no wave) |
| `--spiral` | no break: a target, not a spiral pair |
| `--photo` | the φ term dropped: light does nothing |
| `--clock` | the acid term dropped from the plugin's rate |
| `--sync` | the dose sized from the wrong closed form |
| `--briggs` | the plugin's malonic acid step at half its constant |
| `--traffic` | oxygen never re-oxidises the dye (k_ox = 0) |
| `--bluebottle` | the plugin's reduction constant doubled |
| `--chameleon` | the plugin's two constants swapped |
| `--stir` | Stir ignored |
| `--units` | the cell 0.1 mm whatever Dish Width says |
| `--turing` | the inhibitor diffusing as slowly as the activator |

**A cue held from frame 0 pressed the button at frame 0.** The fleet's
`valueAt` holds a track's first key before its frame, which is right for a
slider's initial value and wrong for an event: `2 Drop 1` dropped at frame
0 and the verify step's "frames 0–1 identical" caught it. `runPipe` leaves a
parameter untouched until its first key (its default stands), so a press
lands where it is cued and a `0` key is needed to press again. A cue's name
is the longest run of words that is a parameter (`Drop`, `Drop Size`, `Drop
Position` all exist) and the rest is the value, so `0 Reaction Chemical
Chameleon` reads; a value that is not a number or one of the option's names
is refused (`strtof( "Fixed" )` is silently 0: polyhedral).

**A mutant can hang.** The first transport mutant inverted the lead loop's
test (`while( wait < lead )` → `>`), which never returns, and `mutate.sh`
sat on it for ten minutes with nothing to say. The script now runs each
check under a limit (`MUTANT_LIMIT`, 600 s), reports a hang as a hang (a
failure to pass, so counted as caught, but not what is wanted), and the
transport mutant is now a wrong answer that returns (the wait counted from
the wrong end of the bar). **Fleet-wide**: any mutate.sh without a limit.

### The recorded mutation

`tools/mutate.sh` (run by verify.sh) changes one character of shipped code in
a copy of the tree and requires a named check to fail. **The GLSL mutation of
record**: in the Oregonator's rate, `s.x * ( 1.0 - s.x )` became
`s.x * ( 1.0 + s.x )` — the autocatalysis unbounded — and **`--oregonator`
caught it** (the stirred dish never oscillates). Two more GLSL mutants
(Beer–Lambert's depth added instead of multiplied, caught by `--beer`; the
thiosulfate making iodine instead of taking it, caught by `--clock`) and two
C++ (the transport's wait counted from the wrong end of the bar, caught by
`--transport`; the clock's closed form itself with `2 H0` → `2 + H0`, caught
by `--clock` — a wrong reference is caught too) are caught. This proves the
harness drives the shaders the plugin ships, not a copy of them: it has none.

### The sweep's context table

`tools/sweep.py` sweeps each of the 30 controls where it can act: Catalyst
in BZ (the default); the wave controls (Reset, Seed, Stir, Detail, Drop, Drop
Size, Drop Position, Break Wave, Light Coupling) at Time-lapse 0.8 (256
chemical seconds, so a drop's wave or a soup's targets exist), a Break Wave
pressed at frame 100 once the drop's wave is there (`--set "Name=V@F"`);
Reactor and Clock Sync on the iodine clock; Shake and Audio Shakes on the
blue bottle, where a shake turns the layer blue; the audio controls with a
beat fed in. The first sweep, with the bare BZ context, found one DEAD
control (the Over's Lightbox, withdrawn) and 17 "barely alive" (under 0.05
of an 8-bit level mean difference): a 4 mm drop in a 60 mm dish, a Seed
whose pacemakers had not yet fired, a Break Wave with no wave to break, a
Shake in a reaction that does not care. Each got the context above; none
got a lower floor. Run once with `--no-context` (the table emptied),
2026-10-04: **8 dead** — the source's Drop Size, Drop Position and Clock
Sync; the Over's Drop, Drop Size, Drop Position, Clock Sync and Light
Coupling — and **12 barely alive** (Reactor, Reset, Seed, Audio Shakes in
both; the source's Drop; the Over's Stir, Detail and Shake). Each is a
control whose context the table names (a drop needs a position, a sync
needs a clock, a coupling needs a photosensitive catalyst and a wave on the
clip), which is what the table is for.

## Shape of the code

- `source/Controls.*` — the parameter ids, names, host order (About last; the
  Over's three extras; no Convection), the enums, the geometric mappings and
  the grid.
- `source/Chemistry.*` — the recipes, the models (`MakeBZ`, `MakeLE`, the
  clock's closed forms, the dye family's well-mixed model, the chameleon's
  peak time, the BR kinetics), `FreshState`, `WellMixedRhs` and the stiff
  integrator the references use.
- `source/Spectra.*`, `source/CieTables.h` — the 20 species' Gaussian tables
  with their sources, the named solutions and their hue bands, the lightboxes,
  the primaries, and the CPU `LayerColour` / `LayerMatrix` references.
- `source/Shaders.cpp` — `kCommon` (the #defines `--names` checks), the seed,
  step A, step B, advect, relax, the reductions, the colour pass, the
  composite and the thumbnail.
- `source/Honeydew.*` — the plugin: time, transport, audio, the sync, the
  substep plan, the passes, the test hooks (`Perturb`, `DoseForTest`,
  `SetSubstepScaleForTest`, `State`/`Load`).
- `source/BrEngine.*` — Briggs–Rauscher on the CPU.
- `source/SourcePlugin.cpp`, `EffectPlugin.cpp` — the two registrations.
- `tools/hdtest/main.cpp` — the harness: the rig, the colour classifier, the
  references, every check, the negative controls, the bench, and the fleet's
  `--pipe` / `--film` / `--script` filming modes (raw RGBA, cue sheets).
- `tools/sweep.py`, `tools/mutate.sh`, `tools/verify.sh`, `tools/glslc.sh`,
  `tools/bake_cie.py`.

## What is genuinely verified, and what is assumed

Verified on this machine (Apple Silicon), see the README's Status table for
the numbers: every check above on the GPU at 1280x720 and 320x180, the cheap
set on the software renderer at 320x180; both bundles universal; oxbow probe
reads `SW Honeydew` / `HD01` / source and `SW Honeydew Over` / `HD02` /
effect; oxbow selftest renders 120 frames through each; the 30 controls over
both plugins all move the picture; 20 negative controls detected; 5 mutants
caught.

Assumed, or not done:

- **Never loaded into Resolume**, on any platform. Unknown there: how the 31
  and 33 parameters present, the clock unit, `SetBeatInfo`'s bar phase, the
  FFT bins, whether events arrive as 1 then 0. No Windows build has run.
- **The chemistry is the cited models at the cited constants**, and eleven of
  the twenty spectral peaks and a number of rate and diffusion constants are
  STAND-INS, each marked in CHEMISTRY.md. The plugin is verified against
  those models; the models are not verified against a dish.
- **The closures** (pacemakers, stirring, aeration, Flow's residence, the
  drop's mixing) are models with a stated form; nothing checks them against a
  measurement.
- **Convection** is not built.
- **The hues** the classifier names are bands chosen so the cited solutions
  land in them; a different observer or display gamut would move a boundary.
- **The bench** was taken while other builds may have shared the machine;
  the figures are the idle medians.

## Open design questions

- **Should Flow be the default?** It keeps BZ alive on stage but hides the
  run-down that is half the demonstration (the clock's last snap, the
  valentine's last pink).
- **A Drop that is a reagent choice** (thiosulfate, air, permanganate, a dab
  of catalyst) rather than the reaction's natural dose would let one reaction
  be played several ways.
- **The sync's hold** (a quarter period) is a guess at how long a transition
  must be visible before the next is sized; a musician may want it shorter.
- **Should the Over's Brightest drop position follow the clip's motion**
  (the brightest point moves) or hold the point it chose?
- **Hypomanganate**: if its spectrum and the Mn(VI)→Mn(V) step can be cited,
  the blue is one more species in the chain and the spectra pipeline already
  carries it.
- **The chemoconvection solver** (IDEAS.md) and whether a depth-resolved
  layer belongs in an FFGL plugin at all, against a 2-D closure that reads
  the same.
