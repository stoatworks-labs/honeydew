# The chemistry, and where every number came from

Every rate constant, diffusion coefficient, absorption peak, molar absorptivity,
band width and equilibrium constant the plugin uses, with its source. Where a
number could not be found in a citable source from this build, it is marked
**STAND-IN** and the reason is given; nothing invented is presented as measured.
The code these numbers live in is `source/Chemistry.h/.cpp` (kinetics, recipes,
closed forms) and `source/Spectra.cpp` (spectra, lightboxes, primaries).

Units in the code: mol/L and seconds for chemistry; millimetres for the dish
(diffusion coefficients are converted from cm²/s, ×100); M⁻¹ cm⁻¹ and nm for
spectra, with the layer depth in cm.

## The light

- **Colour matching functions**: CIE 1931 2° observer at 10 nm from 380 to
  780 nm (41 samples), taken from the 1 nm table in the supplemental repository
  of Wyman, Sloan & Shirley, *Simple Analytic Approximations to the CIE XYZ
  Color Matching Functions*, JCGT 2(2) 2013 (`data/ciexyzCurves1931_1nm.h`,
  credited there to the RIT Munsell Color Science Laboratory tables). Baked by
  `tools/bake_cie.py` into `source/CieTables.h`. The analytic fit of that paper
  is NOT used; the table is.
- **Illuminant D65**: CIE 15:2004 Table T.1 at 5 nm, as tabulated by
  colour-science (`colour/colorimetry/datasets/illuminants/sds.py`), sampled at
  the same 41 wavelengths. Through the baked tables D65 lands at xy (0.3127,
  0.3291) against CIE's (0.3127, 0.3290), and sRGB (0.9994, 1.0003, 0.9991).
- **XYZ → linear sRGB**: IEC 61966-2-1's matrix (3.2406, −1.5372, −0.4986; …).
- **LED 5000K and Warm White lightboxes**: MODELS, not measurements: a 450 nm
  pump (σ 10 nm) plus a broad phosphor band (570 nm, σ 60 nm; warm: 452 nm pump
  at a third the weight and a 600 nm, σ 65 nm phosphor). Their whites are not
  normalised to sRGB white: they are warmer, which is the point of choosing them.
- **The Over's display primaries**: a MODEL of an LED panel: R 630 nm σ 12,
  G 530 nm σ 18, B 460 nm σ 11 (Gaussians). The filter matrix is normalised so
  that an empty dish is exactly the identity, so the model only decides how a
  coloured layer filters a coloured clip.
- **Beer–Lambert**: T(λ) = 10^(−Σ εᵢ(λ) cᵢ ℓ). The GPU sums 41 samples in float;
  `hdtest --beer` holds the pixels to the double integral to 2×10⁻⁴ and shows
  that doubling the depth is the polychromatic integral, not the per-channel
  square (the two differ by 0.3 of a unit colour for a ferroin dish).

### Spectra: a sum of Gaussians each

The first Gaussian of each sum is the cited peak (λmax, εmax); the rest shape
the band, and every width is a fit unless a source gave it. `hdtest --spectra`
checks that each sum reproduces its cited peak to 2% and that each named
solution lands in its named hue.

| species | λmax, εmax (M⁻¹ cm⁻¹) | source | status |
| --- | --- | --- | --- |
| ferroin Fe(phen)₃²⁺ | 510 nm, 1.11×10⁴ | the classic analytical value (e.g. the ferroin method for Fe(II)); Wikipedia gives 511 nm | cited; the 440 nm shoulder (4.5×10³) and widths are a fit to the band's known breadth (400–550 nm) |
| ferriin Fe(phen)₃³⁺ | 600 nm, 870 | a pale blue; λmax 590–600 nm is widely stated | **STAND-IN** ε: from memory of BZ imaging work. A web summary's "7200 at 593 nm" was found and NOT used: a solution that pale cannot have it |
| Ru(bpy)₃²⁺ | 452 nm, 1.46×10⁴ | Kalyanasundaram, Coord. Chem. Rev. 46, 159 (1982), and many since | cited; 400 nm shoulder a fit |
| Ru(bpy)₃³⁺ | 670 nm, 420; 420 nm, 3000 | "a broad weak band at 670 nm, much weaker than the 2+" (Faraday Discuss. 2019 and others) | **STAND-IN** both heights (Kalyanasundaram 1982 from memory) |
| Ce(IV) in H₂SO₄ | 320 nm, 5580 | the ceric sulfate dosimeter's value | **STAND-IN** status (from memory, not read in the paper); the visible yellow is its fitted tail (σ 58 nm) |
| I₂ (aq) | 460 nm, 746 | Awtrey & Connick, JACS 73, 1842 (1951) | cited |
| I₃⁻ | 353 nm, 26 400; 288 nm, 40 000; 460 nm, 975 | Awtrey & Connick 1951 (the 353 and 288 bands are quoted everywhere; the weak 460 band from memory of their table) | the 460 band is **STAND-IN** status |
| starch–triiodide | 620 nm, 4×10⁴ per bound I₃⁻ | the amylose–polyiodide complex absorbs at 600–620 nm (the preprints.org review of the iodine/iodide/starch complex, 2023; Bailey & Whelan and later) | λ cited, ε **STAND-IN** |
| ClO₂ (aq) | 359 nm, 1250 | the standard analytical value (several sources) | cited |
| indigo carmine, acid form (blue) | 610 nm, 9.0×10³ | PMC9369777 (2022) measured 9×10³; literature 8.1×10³–1.17×10⁴ | cited |
| indigo carmine, alkaline form (yellow) | 430 nm, 5×10³ | the dianion above the pKa; isosbestic points at 494 and 656 nm are reported (PCCP 2012) | **STAND-IN** λ and ε |
| indigo carmine semiquinone (red) | 500 nm, 2×10⁴ | J. Chem. Soc. Faraday Trans. 90, 2525 (1994) has the stopped-flow spectrum of the one-electron intermediate; not reachable from this build | **STAND-IN** |
| leuco indigo carmine (yellow) | 410 nm, 6×10³ | leuco-indigo absorbs at 407–410 nm in alkali (several sources) | λ cited for indigo, ε **STAND-IN** |
| methylene blue (monomer) | 664 nm, 7.4×10⁴ | omlc.org's tabulated spectrum (Prahl, 10 µM in water: 74 028 at 664 nm, 38 121 at 612 nm); others 8.2×10⁴–9.5×10⁴ | cited |
| methylene blue dimer | 605 nm, 1.32×10⁵ per dimer; K_dimer 2×10³ M⁻¹ | Bergmann & O'Konski, J. Phys. Chem. 67, 2169 (1963) for the band; Wikipedia's table for ε; "K of order 10³ M⁻¹" | K is a **STAND-IN** order of magnitude |
| resazurin | 600 nm, 6×10⁴ | 600–602 nm in alkali is widely stated; the alamarBlue assay table gives 1.17×10⁵, others less | ε **STAND-IN** |
| resorufin | 571 nm, 7.3×10⁴ | Sigma product sheet R3257 (pH 8); reports 5.4×10⁴–9.2×10⁴ | cited; the 540 nm shoulder and a 480 nm wing are a fit that makes it pink rather than magenta |
| permanganate | 526 nm, 2.40×10³; 546 nm, 2.38×10³ | spectrophotometric analyses of KMnO₄ (the vibronic pair; the 507 and 566 shoulders) | cited; four Gaussians whose sum is 2414 and 2389 at the two peaks |
| manganate MnO₄²⁻ | 606 nm, 1700; 439 nm, 1400 | the 606 and 435–439 nm bands are reported in alkaline permanganate kinetics (e.g. the κ-carrageenan study) | ε **STAND-IN** |
| colloidal MnO₂ | 380 nm, 5000 per Mn, σ 110 | Pérez-Benito and co-workers follow soluble colloidal MnO₂ at 418 nm; the extinction rises into the UV | **STAND-IN** shape and height (extinction, scattering included) |

## The recipes (the 1× molarities each Recipe slider multiplies)

| reaction | Oxidant | Acid or Base | Reductant | Indicator |
| --- | --- | --- | --- | --- |
| Belousov–Zhabotinsky | NaBrO₃ 0.3 M | H⁺ 0.3 M | malonic acid 0.1 M | catalyst 1.0 mM |
| Briggs–Rauscher | KIO₃ 0.067 M (H₂O₂ 1.3 M follows it) | H⁺ 0.08 M | malonic acid 0.05 M; Mn²⁺ 0.02 M fixed | starch 5×10⁻⁴ M of sites |
| Iodine clock | H₂O₂ 0.1 M | H⁺ 0.05 M | Na₂S₂O₃ 5 mM; KI 0.05 M fixed | starch 5×10⁻⁴ M of sites |
| CDIMA Turing | ClO₂ 2.0 mM | H⁺ 1.0 mM | malonic acid 3.5 mM | starch/PVA 1.5 mM of sites |
| Traffic light | O₂ 2.6×10⁻⁴ M (air saturation) | NaOH 0.08 M | glucose 0.054 M | indigo carmine 2×10⁻⁴ M |
| Blue bottle | O₂ 2.6×10⁻⁴ M | NaOH 0.08 M | glucose 0.054 M | methylene blue 4.6×10⁻⁵ M |
| Vanishing valentine | O₂ 2.6×10⁻⁴ M | NaOH 0.08 M | glucose 0.054 M | resazurin 3×10⁻⁵ M |
| Chemical chameleon | KMnO₄ 1.3 mM per dose | NaOH 0.25 M | glucose 0.05 M | KMnO₄ 1.3 mM in the dish at the start |

The BZ recipe is a thin-layer ferroin dish of the Winfree / Zaikin–Zhabotinsky
proportions; the BR recipe is Shakhashiri's proportions, which sit inside the
transcribed De Kepper–Epstein model's batch-oscillatory region (below); the
blue-bottle family is Pons et al. 2000's glucose and (for methylene blue) dye,
with four times their [OH⁻] so the ¼× slider reaches their 0.020 M and 4× the
classical demonstration's ~0.3 M; the dye strengths for indigo carmine and
resazurin are demonstration strengths chosen to absorb about one absorbance
unit in a 7 mm layer. "Starch sites" are a **STAND-IN** unit: the molarity of
amylose helix sites that bind one I₃⁻ each, for 1 g/L of starch.

## Belousov–Zhabotinsky

The Field–Körös–Noyes mechanism as the Oregonator, with Tyson's scaling, kept
as **three variables** on the GPU (HBrO₂, Br⁻, the oxidised catalyst):

| constant | value | source |
| --- | --- | --- |
| k₁ (A + Y → X + P) | 2.0 M⁻³ s⁻¹ | Field & Försterling, J. Phys. Chem. 90, 5400 (1986), as tabulated in Table 1 of Ipsen, Kramer & Sørensen, arXiv:chao-dyn/9711004 |
| k₂ (X + Y → 2P) | 3.0×10⁶ M⁻² s⁻¹ | same |
| k₃ (2X → A + P) | 3.0×10³ M⁻¹ s⁻¹ | same |
| k₄ (A + X → 2X + 2Z) | 42 M⁻² s⁻¹ | same |
| k_c (B + Z → f Y, k₅ = k_c [MA]) | 1 M⁻¹ s⁻¹ | **STAND-IN**: Tyson's order-of-magnitude recommendation, from memory; the Ipsen table's k₅ = 0.167 s⁻¹ is a lumped value for their recipe |
| f | the **Excitability** control, 1 to 4 (geometric), default 1.4; the harness's excitable waves at 2.6 | a model parameter, not derivable from the recipe (Tyson; Jahnke & Winfree 1991). Regimes, below | 
| D(HBrO₂) | 1.0×10⁻⁵ cm²/s | Hynne & Sørensen 1993 via the same table |
| D(Br⁻) | 1.6×10⁻⁵ cm²/s | same |
| D(catalyst) | 0.6×10⁻⁵ cm²/s | same |

Scaling: ε = k₅/(k₄AH), ε′ = 2k₃k₅/(k₂k₄AH²), q = 2k₁k₃/(k₂k₄), T₀ = 1/k₅,
X₀ = k₄AH/(2k₃), Z₀ = (k₄AH)²/(k₃k₅). At the 1× recipe: ε 0.0265, ε′ 1.8×10⁻⁴,
q 9.5×10⁻⁵, T₀ 10 s. **Why three variables**: q is of the order of ε′ here, so
the bromide relaxes at (q + x)/ε′ ≈ 1 at rest, not fast, and the Tyson–Fife
two-variable reduction misses the period by 23% (`hdtest --oregonator` states
it: 77.6 s against 101.0 s).

- **The regimes of f** (0.1.1). Jahnke & Winfree 1991 (J. Phys. Chem. 95, 4910) map
  the two-variable Oregonator's behaviour over f and ε: the rest state is
  unstable (the medium oscillates) between f ≈ 1/2 and 1 + √2 ≈ 2.414 for
  q → 0, and excitable (a stable rest state that a finite kick fires, where a
  broken wave winds a spiral) beyond. **This three-variable model at the 1×
  recipe stops oscillating at f = 1.78** (bisection on the ODE from a 1% kick,
  two catalyst peaks in 200 s; `chem::BZHopfF`), at 2.16 with twice the acid,
  and at ½× acid it does not oscillate at any f ≥ 1: the boundary moves with ε
  and ε′, and the textbook 1 + √2 is not this model's. The default 1.4 is the
  0.1.0 dish (oscillatory); the harness's wave checks run at 2.6, where the
  dish is excitable at every acid they use. The browser demo's read-out asks
  the plugin which regime the sliders' recipe is in.
- **A fresh dish** starts with HBrO₂ at 1.01 × its rest value (`kFreshKick`):
  the rest state is an unstable fixed point in the oscillatory regime, and a
  dish seeded exactly on it and stirred hard (the whole-vessel relaxation holds
  the dish's mean to float rounding) stayed there for 1500 s in 0.1.0. The same
  1% kick is what the harness's references start from. In the excitable regime
  it decays, as a sub-threshold kick should.
- **Field & Noyes 1974** (JACS 96, 2001): v = 0.04 cm s⁻¹ M⁻¹ √([H⁺][BrO₃⁻]),
  0.120 mm/s at the 1× recipe. The pulled-front limit 2√(D k₄ A H) with the
  constants above is 0.123 mm/s. The model's excitable trigger wave (f = 2.6)
  is a pushed front at 0.10 mm/s on 0.1 mm cells (0.087 mm/s on the 1-D double
  line at 0.025 mm cells), and its acid exponent is 0.80, not 0.5: the √ law is
  the pulled-front limit, which the Oregonator's excitable rest state is not in.
  Reported, not gated.
- **Photo-Oregonator**: the light term φ in the bromide equation (Krug, Pohlmann
  & Kuhnert, J. Phys. Chem. 94, 4862 (1990); Kádár, Amemiya & Showalter, J. Phys.
  Chem. A 101, 8200 (1997)). φ at full light is 0.06 (dimensionless), a
  **STAND-IN** scale (arXiv:1601.00848 finds wave extinction above φ ≈ 0.024 at
  f = 1.16); the harness measures this model's own threshold (a 1.6 mm band
  stops a wave between φ 0.0014 and 0.0019 at f = 2.6) and gates on it.
- **Pacemakers** (dust): sites where the chemistry runs 30% faster, one per
  24×24-cell block with probability 0.3 — a **STAND-IN** heterogeneity model.
- **Batch**: the fuel is spent at a rate that empties a dish in about an hour
  (**STAND-IN**: the Oregonator's lumped step 5 would spend the malonic acid in
  minutes, which a real dish does not).
- **Flow**: a CSTR residence of 300 s (**STAND-IN**) for every reaction but BR.

## Briggs–Rauscher

De Kepper & Epstein, JACS 104, 49 (1982): the ten-step mechanism with its rate
constants **as transcribed by Binous (a Wolfram Demonstration) and the Paredes
thesis (University of Florida)**, the only copy reachable from this build. The
chain of custody is therefore: paper → Binous's notebook → the thesis's
transcription → here. The constants (r₁ 1.43×10³, r₂ 2×10¹⁰, r₃ 3.1×10¹²,
r₋₃ 2.2, r₄ 7.3×10³, r₋₄ 1.7×10⁷, r₅ 6×10⁵, r₆ 1×10⁴, r₋₆ 1×10⁴, r₇ 3.2×10⁴,
r₈ 7.5×10⁵, r₉ 40, r₁₀ 37, C₉ 10⁴) and the species mapping are in
`Chemistry.h`. Two things the transcription did not settle: the units of each
constant (taken as the mass-action units of each step) and whether the
manganese total is the Binous recipe's 0.004 M (the 1× recipe here uses 0.02 M,
a demonstration's). What the transcription DOES reproduce: batch oscillations
at demonstration recipes (period 248 s at the 1× recipe, eleven oscillations,
then it runs down after 42 minutes), the colourless → amber → blue-black order
at 7 mm, and sustained oscillation in a CSTR at a residence of 800 s (1/156 s
and 1/400 s quench it in the model; the 800 s is the plugin's Flow for BR).
`hdtest --briggs` holds the plugin's period to the mechanism in double to 1%.

- D for every iodine species between the CPU grid's cells: 1.5×10⁻⁵ cm²/s,
  **STAND-IN** (used only for the texture the stirring makes).

## The iodine clock

| constant | value | source |
| --- | --- | --- |
| k₁ (acid-independent) | 0.0115 M⁻¹ s⁻¹ | Liebhafsky & Mohammad, JACS 55, 3977 (1933), as commonly quoted (0.69 M⁻¹ min⁻¹); the paper's own table was not reachable: **STAND-IN** status until read there |
| k₂ (acid-dependent) | 0.175 M⁻² s⁻¹ | same (10.5 M⁻² min⁻¹) |
| I₂ + 2 S₂O₃²⁻ | 7.8×10⁹ M⁻¹ s⁻¹ | Scheper & Margerum, Inorg. Chem. 31, 5466 (1992); treated as instantaneous (an exact titration each substep) |
| K(I₂ + I⁻ ⇌ I₃⁻) | 722 M⁻¹ | Ramette & Sandford, JACS 87, 5001 (1965) |
| K_starch (Langmuir site binding of I₃⁻) | 10⁵ M⁻¹ | **STAND-IN**: the complex is known to be very stable; no site constant was found to cite |
| D(H₂O₂) | 1.4×10⁻⁵ cm²/s | **STAND-IN** (a small molecule's order) |
| D(I⁻) | 2.0×10⁻⁵ cm²/s | the CRC limiting ionic value, from memory: **STAND-IN** status |
| D(I₂) | 1.36×10⁻⁵ cm²/s | Cantrel et al. 1997 report values near this: **STAND-IN** status |
| D(S₂O₃²⁻) | 1.1×10⁻⁵ cm²/s | **STAND-IN** |

Closed forms (Batch): t* = −ln(1 − S₀/2H₀)/(k′ I₀); (Flow, feed k₀ at Hfeed):
S(t) = 2k′I₀[Hs(e^{k₀t} − 1)/k₀ + (H₀ − Hs)(1 − e^{−k′I₀t})/(k′I₀)] with
Hs = Hfeed k₀/(k₀ + k′I₀). `hdtest --clock` holds the pixel snap to these at
three recipes in both reactors; `--sync` lands it on the bar at two tempos.

## CDIMA Turing

Lengyel & Epstein, Science 251, 650 (1991), with the kinetic constants of the
CIMA/CDIMA system at 4 °C as tabulated (quoting Lengyel, Rábai & Epstein) in
Table 3 of arXiv:2504.02530, and the diffusion coefficients listed there:

| constant | value |
| --- | --- |
| k₁ₐ | 6.2×10⁻⁴ s⁻¹ |
| k₁ᵦ | 5×10⁻⁵ M |
| k₂ | 900 M⁻¹ s⁻¹ |
| k₃ₐ | 100 M⁻² s⁻¹ |
| k₃ᵦ | 9.2×10⁻⁵ s⁻¹ |
| α | 10⁻¹⁵ M² (the model's cut-off) |
| K = k₄/k₋₄ (starch–triiodide) | 10⁸ M⁻¹ |
| D(I⁻), D(ClO₂⁻) | 0.7×10⁻⁵, 0.75×10⁻⁵ cm²/s |
| [I₂] in the fed gel | 1.0×10⁻³ M (the paper's estimate from the KI feed) |

Dimensionless: a = k₁ₐ[MA][I₂]/(√α k₂[ClO₂](k₁ᵦ + [I₂])), b = k₃ᵦ[I₂]/(√α k₂[ClO₂]),
σ = 1 + K[S][I₂], d = D(ClO₂⁻)/D(I⁻), t′ = k₂[ClO₂] t, x′ = √(k₂[ClO₂]/D(I⁻)) x.
At the 1× recipe: a 36.3, b 1.62, σ 151, d 1.07, t′ = 0.556 s, x′ = 0.0197 mm;
the Turing wavelength 2π/k_c is 0.108 mm, the Hopf line is at σ 13. The
malonic acid is 3.5 mM rather than the table's 2.25 mM because at 2.25 mM the
reduced model's steady state is 10% outside its Turing region. The light term
w (Muñuzuri, Dolnik, Zhabotinsky & Epstein, JACS 121, 8065 (1999)) at full
light is a/4, a **STAND-IN** scale. Experiment: Castets, Dulos, Boissonade & De
Kepper 1990 and Ouyang & Swinney 1991 report ~0.2 mm.

## The blue-bottle family

Pons, Sagués, Bees & Sørensen, J. Phys. Chem. B 104, 2251 (2000), their
kinetics for methylene blue: V₁ = k₁[O₂][MBH], **k₁ = 2000 M⁻¹ s⁻¹**;
V₂ = k_obs[MB⁺], **k_obs = k₂[OH⁻][GL] = 0.0042 s⁻¹** at [OH⁻] 0.020 M,
[GL] 0.054 M, so k₂ = 3.89 M⁻² s⁻¹ (derived); O₂ saturation 2.6×10⁻⁴ M
(air-saturated water; Benson & Krause 1984 give 8.26 mg/L = 2.58×10⁻⁴ M at
25 °C); D(O₂) 2.11×10⁻⁵, D(dye) 4×10⁻⁶, D(glucose) 6.7×10⁻⁶ cm²/s (Pons 2008
Table I). This is the account chosen for the Blue Bottle; the other two are
recorded here: Engerer & Cook, J. Chem. Educ. 76, 1519 (1999) teach a rate
first order in glucose, dye and hydroxide and zero order in oxygen (the same
form as Pons's V₂, which is why Pons's measured constants are used); Anderson,
Wittkopp, Painter et al., J. Chem. Educ. 89, 1425 (2012) find the sugar's
enolisation (2.3×10⁻³ min⁻¹ at 0.184 M glucose, pH 13.3, 25 °C) rate-limiting,
with O₂ consumption about 60% of the enolisation rate — a sequence this model
does not resolve. Pons's constants are the ones measured for the system the
pattern paper describes, so they are the ones used.

- **Indigo carmine** (traffic light): the same k₂ applied to each of its two
  one-electron steps (**STAND-IN** by analogy; J. Chem. Educ. 101, 2505 (2024)
  measured the steps, not reachable here), the semiquinone reduced at
  kSemiquinone = 1.0 × the first step's rate (**STAND-IN**). pKa 12.6, chosen
  inside the reported 11.17–12.99 span (two methods) and the 11.4–13.0
  indicator range so the 1× recipe (pH 12.9) reads green: **STAND-IN**.
- **Resazurin** (valentine): resazurin → resorufin irreversibly at the dye rate
  (**STAND-IN** by analogy), then resorufin ⇌ dihydroresorufin as the blue
  bottle. Resorufin's fluorescence is not modelled.
- **Aeration of a still layer**: k_L a = 2D(O₂)/d², a CLOSURE (the mean
  diffusion distance to the middle of a layer of depth d), not a measurement;
  a Shake multiplies it by 200 for a moment and Stir by (1 + 20 Stir²)
  (**STAND-IN** closures). A 1.5 mm layer is fed faster than it consumes at
  the 1× recipe and stays blue, as a thin dish does; a 7 mm layer fades (Pons
  2000's blue top layer is 1–3 mm).
- **Density**: gluconic acid's excess density over glucose, 0.044 g/(cm³ M),
  Pons 2008 Table I (Pons 2000 measured 1.0008 against 1.0032 g/mL at 0.055 M,
  25 °C). Recorded for the chemoconvection model, which is not built in 0.1.0.

## The chemical chameleon

A pseudo-first-order chain MnO₄⁻ → MnO₄²⁻ → MnO₂ (colloid), each step first
order in the Mn species, [OH⁻] and glucose (Odebunmi & Owalude 2008 and others
find first order in substrate, oxidant and OH⁻ for sugars). The constants are
**STAND-INS** calibrated to the demonstration's timings (purple to green in
~40 s, green to brown in ~2 min at 0.25 M NaOH, 0.05 M glucose): k_A 4.0,
k_B 0.7 M⁻² s⁻¹; D(oxyanions) 1.5×10⁻⁵, D(MnO₂ colloid) 10⁻⁷, D(glucose)
6.7×10⁻⁶ cm²/s (the first two **STAND-IN**). The manganate peak lands at
ln(β/α)/(β − α) after a dose, independent of the dose, which is why Clock Sync
times the chameleon's dose rather than sizing it.

**The blue, settled by the spectra**: the purple + green mixture without
hypomanganate does NOT read blue through these spectra: at 25% manganate it is
purple (hue 309, saturation 0.43), at 50% a grey (hue 288, saturation 0.20),
at 75% a grey-green. So this model follows the hypomanganate account by
default, i.e. the blue the demonstration shows would need MnO₄³⁻ (Mn V, near
670 nm), whose kinetics and spectrum were not found to cite, and the plugin
renders the sequence purple → grey → green → yellow-brown. Recorded in
AGENTS.md.
