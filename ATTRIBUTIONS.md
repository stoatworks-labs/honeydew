# Attributions

Honeydew is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Source-plus-Over shape, parameter table, GL state and harness — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

One core registered as a source and an Over effect, HostOrder() with the About block last, the host-clock unit vote, GLState.h, Diag, the harness's rig, cue sheets and --pipe, tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py are conway's (which carries them from radar, boreal, flyback, downpour, tinsel and plotter).

### Audio analyser — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is conway's copy of radar's copy of millpond's copy of rosette's analyser (itself from macroblock's), with its primed first frame, which hdtest --prime checks.

### A float-format PassBuffer — Stoatworks stencil

<https://github.com/stoatworks-labs/stencil>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.{h,cpp} is stencil's, cut down and given RGBA32F and multiple-render-target variants: a texture and a framebuffer whose upload format follows the internal format.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The Oregonator and the Field–Körös–Noyes mechanism — Field, Körös and Noyes; Tyson and Fife; Field and Försterling

Field, Körös and Noyes, J. Am. Chem. Soc. 94, 8649 (1972); Field and Noyes, J. Chem. Phys. 60, 1877 (1974); Tyson and Fife, J. Chem. Phys. 73, 2224 (1980); Field and Försterling, J. Phys. Chem. 90, 5400 (1986). The three-variable Oregonator the plugin integrates per texel, its rate constants, and the trigger-wave speed law the harness reports the plugin's speed against.

### The photo-Oregonator — Krug, Pohlmann and Kuhnert; Kádár, Amemiya and Showalter

Krug, Pohlmann and Kuhnert, J. Phys. Chem. 94, 4862 (1990); Kádár, Amemiya and Showalter, J. Phys. Chem. A 101, 8200 (1997). Light making bromide in the Ru(bpy)3-catalysed layer: the term hdtest --photo checks.

### Briggs–Rauscher — De Kepper and Epstein; Noyes and Furrow

De Kepper and Epstein, J. Am. Chem. Soc. 104, 49 (1982); Noyes and Furrow, J. Am. Chem. Soc. 104, 45 (1982); the mechanism as set out by J. Paredes's thesis (University of Florida) after Binous. The ten-species model the CPU engine integrates and hdtest --briggs holds it to.

### The iodine clock — Liebhafsky and Mohammad

Liebhafsky and Mohammad, J. Am. Chem. Soc. 55, 3977 (1933): the Harcourt–Esson rate law (k1 + k2[H+])[H2O2][I-] whose closed form hdtest --clock and Clock Sync use.

### CDIMA Turing patterns and the Lengyel–Epstein model — Lengyel and Epstein; Castets, Dulos, Boissonade and De Kepper; Ouyang and Swinney; Muñuzuri, Dolnik, Zhabotinsky and Epstein

Lengyel and Epstein, Science 251, 650 (1991); Castets et al., Phys. Rev. Lett. 64, 2953 (1990); Ouyang and Swinney, Nature 352, 610 (1991); Muñuzuri et al., J. Am. Chem. Soc. 121, 8065 (1999). The Turing wavelength and the light term hdtest --turing checks.

### The blue bottle and its family — Pons, Sagués, Bees and Sørensen; Anderson et al.; Engerer and Cook

Pons, Sagués, Bees and Sørensen, J. Phys. Chem. B 104, 2251 (2000): the rate law (k1 = 2000 M-1 s-1, kobs = 0.0042 s-1) the traffic light, blue bottle and vanishing valentine run on. Anderson et al., J. Chem. Educ. 89, 1425 (2012) and Engerer and Cook, J. Chem. Educ. 76, 1519 (1999) as the other accounts. Pons, Batiste and Bees, Phys. Rev. E 78, 016316 (2008) for the chemoconvection this version does not build.

### Absorption spectra — Awtrey and Connick; Bergmann and O'Konski; Pérez-Benito and co-workers; others in docs/CHEMISTRY.md

Awtrey and Connick, J. Am. Chem. Soc. 73, 1842 (1951) (triiodide); Bergmann and O'Konski, J. Phys. Chem. 67, 2169 (1963) (methylene blue and its dimer); Pérez-Benito and co-workers (colloidal MnO2); the ferroin, ferriin, Ru(bpy)3, cerium, indigo carmine, resazurin, resorufin, permanganate and manganate peaks as cited in the repo's docs/CHEMISTRY.md, which marks every stand-in.

### Chemical Demonstrations — Bassam Z. Shakhashiri

Chemical Demonstrations (University of Wisconsin Press): the Briggs–Rauscher and chemical chameleon recipe proportions.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The Belousov–Zhabotinsky reaction and its relatives

Boris Belousov's oscillating reaction (1951) and Anatol Zhabotinsky's work on it, the iodine clock (Harcourt and Esson, 1866), the Briggs–Rauscher reaction (1973), the blue bottle, the traffic light, the vanishing valentine and the chemical chameleon as classroom demonstrations. Implemented from the published mechanisms; nothing is copied from anyone's source.

## Standards and published specifications

What the implementation is measured against.

- **Beer–Lambert law, the CIE 1931 colorimetric system and sRGB** — transmittance T(λ) = 10^(−Σ εi(λ)ci ℓ), integrated against the CIE 1931 2° observer (the 1 nm data as distributed with Wyman, Sloan and Shirley, JCGT 2(2) 2013, from the RIT Munsell Color Science Laboratory's CIE tables) and the D65 illuminant (CIE 15:2004 Table T.1), taken to sRGB (IEC 61966-2-1). tools/bake_cie.py resamples the tables into source/CieTables.h; hdtest --beer holds the pixels to a double-precision integral of the same.
- **ROS2, a second-order L-stable Rosenbrock method** — the per-texel stiff step for the Oregonator and the Lengyel–Epstein model (γ = 1 + 1/√2); the harness's reference integrator is the same scheme in double at a smaller step, checked by Richardson extrapolation.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind the seed noise, the pacemakers, the drop positions and the audio patches.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
