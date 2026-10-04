#pragma once

#include <string>

/**
    The GLSL, as text.

    `kCommon` is a LIBRARY (no #version, no main). Every pass is assembled as
    kVersion + kCommon + its pieces, so the harness runs the text the plugin
    runs (`hdtest` drives the plugin; it has no shaders of its own). Pieces
    stay under MSVC's ~16 KB literal cap; tools/glslc.sh reassembles them.

    The state is two RGBA32F textures per cell, A and B (eight channels), whose
    meaning is the reaction's (Chemistry.h, FreshState). Every pass that
    steps the chemistry reads them with texelFetch; the stir pass alone reads
    between texels (bilinear), because it moves the fluid.

    Passes:
      seed       (Reset) the fresh reagents, the Seed's heterogeneity, the clip's excitation (Over)
      step       one chemistry substep: diffusion (no-flux at the frame or the dish wall), drops,
                 the reaction, in that order
      advect     the stir bar's vortex, semi-Lagrangian (exact rotation), once a frame
      relax      the whole-vessel relaxation to the mean, once a frame
      columns, total   the grid's mean, two reductions
      colour     Beer-Lambert through the layer at the grid's resolution: the source's RGB, or the
                 Over's 3x3 filter matrix (I + sum W_k ( T_k - 1 ): exactly I for an empty dish)
      composite  the picture: the lightbox through the layer, or the clip through it
      thumb      a small copy of the clip, for Drop Position Brightest
*/
namespace honeydew::shaders
{
extern const char* const kVersion;
extern const char* const kCommon;
extern const char* const kStateCommon;

extern const char* const kQuadVertex;
extern const char* const kSeedFragment;
extern const char* const kStepFragmentA;
extern const char* const kStepFragmentB;
extern const char* const kAdvectFragment;
extern const char* const kRelaxFragment;
extern const char* const kColumnsFragment;
extern const char* const kTotalFragment;
extern const char* const kColourFragment;
extern const char* const kCompositeFragment;
extern const char* const kThumbFragment;

/// kVersion + kCommon + the pieces, in order (nullptr skipped).
std::string Assemble( const char* a, const char* b = nullptr, const char* c = nullptr );

} // namespace honeydew::shaders
