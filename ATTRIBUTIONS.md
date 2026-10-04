# Attributions

Honeydew is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is a hand copy in the shape of the fleet's generated file — the master lists live
in the `stoatworks-backend` repo and are pushed out by `scripts/sync-attributions.py`;
until this project is registered there, edit it here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form
without their work.

### Source-plus-Over shape, parameter table, GL state and harness — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect, HostOrder() with the About block
last, the host-clock unit vote, GLState.h, Diag, the harness's rig, tools/verify.sh,
tools/mutate.sh, tools/glslc.sh and tools/sweep.py are conway's (which carries them from
radar, boreal, flyback, downpour, tinsel and plotter).

### Audio analyser — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is conway's copy of radar's copy of millpond's copy of rosette's
analyser (itself from macroblock's), with its primed first frame, which hdtest --prime
checks.

### A float-format PassBuffer — Stoatworks stencil

<https://github.com/stoatworks-labs/stencil>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.{h,cpp} is stencil's, cut down and given RGBA32F and
multiple-render-target variants: a texture and a framebuffer whose upload format
follows the internal format.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers —
there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched
separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly —
listed because it is present in the checkout.

## Data this project carries

### CIE 1931 2° colour matching functions and the D65 illuminant — CIE

The 1931 2° observer at 1 nm, from the data file of Wyman, Sloan and Shirley, "Simple
Analytic Approximations to the CIE XYZ Color Matching Functions", JCGT 2(2) 2013
(which credits the RIT Munsell Color Science Laboratory's online CIE tables,
cis.rit.edu), and the D65 relative spectral power distribution (CIE 15:2004, Table
T.1, as tabulated by the colour-science Python package), resampled to 41 points from
380 to 780 nm by tools/bake_cie.py into source/CieTables.h. These are the observer and the daylight
every colour in the plugin is integrated against; the harness's --spectra checks that
D65 through an empty dish is sRGB white.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is
worth saying out loud. docs/CHEMISTRY.md gives each constant's table and marks the
stand-ins.

### The Oregonator and the Field–Körös–Noyes mechanism

Field, Körös and Noyes, J. Am. Chem. Soc. 94, 8649 (1972); Field and Noyes, J. Chem.
Phys. 60, 1877 (1974); Tyson and Fife, J. Chem. Phys. 73, 2224 (1980); Field and
Försterling, J. Phys. Chem. 90, 5400 (1986). The three-variable Oregonator the plugin
integrates per texel, the rate constants, and the trigger-wave speed law the harness
reports its speed against.

### The photo-Oregonator

Krug, Pohlmann and Kuhnert, J. Phys. Chem. 94, 4862 (1990); Kádár, Amemiya and
Showalter, J. Phys. Chem. A 101, 8200 (1997). Light making bromide in the Ru(bpy)₃
catalysed layer: the φ term `--photo` checks.

### Briggs–Rauscher — De Kepper and Epstein

De Kepper and Epstein, J. Am. Chem. Soc. 104, 49 (1982); Noyes and Furrow, J. Am. Chem.
Soc. 104, 45 (1982); the mechanism as set out by Paredes (Ph.D. thesis, University of
Florida) after Binous. The ten-species model the CPU engine integrates and `--briggs`
holds it to.

### The iodine clock — Liebhafsky and Mohammad

Liebhafsky and Mohammad, J. Am. Chem. Soc. 55, 3977 (1933). The Harcourt–Esson rate law
(k₁ + k₂[H⁺])[H₂O₂][I⁻] whose closed form `--clock` and Clock Sync use.

### CDIMA and the Lengyel–Epstein model

Lengyel and Epstein, Science 251, 650 (1991); Castets, Dulos, Boissonade and De Kepper,
Phys. Rev. Lett. 64, 2953 (1990); Ouyang and Swinney, Nature 352, 610 (1991); Muñuzuri,
Dolnik, Zhabotinsky and Epstein, J. Am. Chem. Soc. 121, 8065 (1999); arXiv:2504.02530
(the gel-reactor concentrations). The Turing wavelength and the light term `--turing`
checks.

### The blue bottle — Pons, Batiste and others

Pons, Batiste et al., J. Phys. Chem. A 104, 2251 (2000) and Pons et al. (2008): the
rate law (k₁ = 2000 M⁻¹ s⁻¹, k_obs = 0.0042 s⁻¹) the dye family runs on, and the
chemoconvection the plugin does not yet build. Anderson et al., J. Chem. Educ. 89, 1425
(2012) and Engerer and Cook, J. Chem. Educ. 76, 1519 (1999) as the other accounts.

### Spectra

Awtrey and Connick, J. Am. Chem. Soc. 73, 1842 (1951) (triiodide); Bergmann and
O'Konski, J. Phys. Chem. 67, 2169 (1963) (methylene blue and its dimer); Pérez-Benito
and co-workers (colloidal MnO₂); the ferroin, ferriin, Ru(bpy)₃, cerium, indigo carmine,
resazurin, resorufin, permanganate and manganate peaks as cited in docs/CHEMISTRY.md,
with the stand-ins marked there.

### Chemical demonstrations — Shakhashiri

Bassam Z. Shakhashiri, Chemical Demonstrations (University of Wisconsin Press). The
Briggs–Rauscher and chameleon recipe proportions.

## Standards and published specifications

What the implementation is measured against.

- **Beer–Lambert law and the CIE 1931 colorimetric system** — transmittance
  T(λ) = 10^(−Σ εᵢ(λ)cᵢℓ), integrated against the 2° observer and taken to sRGB (IEC
  61966-2-1) through the D65 white. The harness's --beer holds the pixels to a
  double-precision integral of the same.
- **ROS2 (Rosenbrock, second order, L-stable, γ = 1 + 1/√2)** — the per-texel stiff
  step for the Oregonator and the Lengyel–Epstein model; the reference integrator the
  harness uses is the same scheme in double at a smaller step, checked by Richardson.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind the seed noise, the
  pacemakers, the drop positions and the audio patches.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you
would rather not be listed — open an issue and it will be fixed.
