# honeydew — ideas not in 0.1.0

What the spec and the build set aside, with the reason, so a later version can
pick them up with the context intact. None of these is promised.

## From the spec's "Not in 0.1.0" list

- **Chemoconvection** (the spec's third change): a depth-resolved Boussinesq
  solver for the blue-bottle family in a still layer, with a Convection
  control, `--rayleigh` (Rayleigh–Bénard between a no-slip bottom and a
  stress-free top: Ra_c 1100.65, k_c·d 2.682, Chandrasekhar 1961) and
  `--chemoconvection` (Pons 2000's 7 mm layer forms cells, 3 mm does not,
  zero densification does not; onset against the 2008 paper's ~71 min). Not
  built; the control is declared by neither plugin (AGENTS.md). The 2-D
  state would need a third dimension or a modal reduction (two or three
  vertical modes per column) to stay inside an FFGL frame budget.
- **Convection for the other reactions**: buoyancy and Marangoni effects on
  BZ waves are real (the "mosaic" patterns in thin layers).
- **Marangoni flow** in the blue-bottle family (the surface tension of the
  oxidised layer).
- **Resorufin's fluorescence**: the vanishing valentine glows pink-orange
  under a blue lightbox; an emission term in the colour pass, fed by the
  absorbed light.
- **MnO₂ flocculation and settling**: the chameleon's colloid clears from the
  top down in a real beaker (a depth-resolved sediment, or a decay of the
  colloid's extinction with a settling time).
- **Landolt and iodate–arsenous acid fronts**: propagating clock fronts, the
  classic "chemical wave" that is not oscillatory.
- **Temperature** (Arrhenius on every constant; a Temperature control from
  10 to 40 °C).
- **CO₂ bubbles** in BZ (the malonic acid's oxidation makes them; a nucleation
  closure and a bubble sprite layer).
- **An OpenFX port**, **a browser demo**, **the user guide**.

## Found while building

- **Hypomanganate** as a fourth chameleon species, if its spectrum and the
  Mn(VI)→Mn(V) step can be cited: the pipeline carries it as one more
  Gaussian table and one more link in the chain.
- **A Drop that chooses its reagent** (thiosulfate, air, permanganate,
  catalyst, bromide) rather than the reaction's natural dose.
- **A tiled Briggs–Rauscher on the GPU in float** with a stiffness-aware
  substep, so BR can have waves and drops; the CPU engine is stirred-only.
- **Spiral seeding** by a drawn line (the Over's clip as the pipette) rather
  than the Break Wave bar.
- **An Exposure that follows the lightbox** so Warm White and D65 land at the
  same luminance.
- **The sync's hold as a control**, and a **Clock Sync that targets the
  snap's end** (the return to colourless) as well as its start.
- **Depth as a map** (a tilted dish, a meniscus): a depth texture the colour
  pass reads, so the edge of a Petri dish reads thinner.
