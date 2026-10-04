#pragma once

#include "CieTables.h"
#include "Controls.h"

#include <array>
#include <vector>

/**
    The light: every coloured species as a molar absorption spectrum
    epsilon( lambda ) in M^-1 cm^-1, the lightboxes, the display primaries,
    the CIE 1931 2-degree observer, sRGB.

    Colour is never a palette. A layer of depth l (cm) holding species i at
    c_i (M) transmits T( lambda ) = 10^( -sum_i eps_i( lambda ) c_i l ), and
    the picture is the light behind it through T, integrated against the
    observer, in sRGB. Each spectrum is a sum of Gaussians fitted to CITED
    peaks (lambda_max, eps_max); the widths are the stated approximation
    unless a source gave them, and every STAND-IN is marked.

    The GPU integrates at the 41 samples of CieTables.h in float; the
    references here integrate the same samples in double (--spectra, --beer).
*/
namespace honeydew::spectra
{
struct Gaussian
{
	double nm;   ///< centre
	double eps;  ///< height, M^-1 cm^-1
	double sigma;///< nm
};

/// Every absorbing species the eight reactions can show. The order is the
/// index the colour pass uses: never reorder without the shader.
enum Species
{
	S_FERROIN = 0,///< Fe(phen)3 2+, red
	S_FERRIIN,    ///< Fe(phen)3 3+, pale blue
	S_RU2,        ///< Ru(bpy)3 2+, orange
	S_RU3,        ///< Ru(bpy)3 3+, pale green
	S_CE4,        ///< Ce(IV) in sulfuric acid, yellow (Ce(III) does not absorb in the visible)
	S_I2,         ///< iodine, aqueous
	S_I3,         ///< triiodide
	S_STARCH_I3,  ///< the amylose-polyiodide complex, blue-black
	S_CLO2,       ///< chlorine dioxide
	S_IC_BLUE,    ///< indigo carmine, oxidised, the acid form (blue)
	S_IC_YELLOW,  ///< indigo carmine, oxidised, the alkaline form (yellow)
	S_IC_SEMI,    ///< indigo carmine semiquinone (red)
	S_IC_LEUCO,   ///< leuco indigo carmine (yellow)
	S_MB,         ///< methylene blue monomer
	S_MB_DIMER,   ///< methylene blue dimer (per dimer)
	S_RESAZURIN,  ///< blue
	S_RESORUFIN,  ///< pink
	S_MNO4,       ///< permanganate, purple
	S_MNO4_2,     ///< manganate, green
	S_MNO2,       ///< colloidal manganese dioxide, yellow-brown
	S_COUNT
};
const char* SpeciesName( Species s );
/// The cited peak each species is fitted to: lambda_max and eps_max (the
/// first Gaussian of its sum), and whether the number is a stand-in.
struct Citation
{
	double lambdaMax;
	double epsMax;
	bool standIn;
	const char* source;
};
const Citation& CitationOf( Species s );
/// The species' Gaussians.
const std::vector< Gaussian >& GaussiansOf( Species s );
/// epsilon( lambda ) in M^-1 cm^-1.
double Epsilon( Species s, double nm );

/// The ones the eight reactions show, for --spectra's hue test: a solution
/// of the species at a concentration and depth that absorbs noticeably, and
/// the hue it should land in.
struct NamedSolution
{
	Species species;
	double molar;
	double depthMm;
	const char* hue;///< red, orange, yellow, green, blue, purple, pink, violet, grey
	double hueLo, hueHi;///< degrees, the accepted band (wraps at 360)
};
const std::vector< NamedSolution >& NamedSolutions();

//---------------------------------------------------------------------------
// Light sources.
//---------------------------------------------------------------------------
/// The lightbox's relative spectral power at the k-th sample. D65 is the CIE
/// table; LED 5000K and Warm White are MODELS (stated in CHEMISTRY.md): a
/// 450 nm pump plus a broad phosphor band, with the phosphor's weight set so
/// the correlated colour temperature is about the name's.
double LightboxPower( Lightbox box, int k );
/// A display primary's emission spectrum at the k-th sample: a MODEL of an
/// LED panel's bands (R 630 nm, G 530 nm, B 460 nm, sigma 12/18/11 nm).
double PrimaryPower( int primary, int k );

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
/// XYZ -> linear sRGB (IEC 61966-2-1, D65).
void XYZToLinearSRGB( const double xyz[ 3 ], double rgb[ 3 ] );
/// Linear sRGB -> hue in degrees (0 red, 120 green, 240 blue), saturation and
/// value, for the hue tests.
void RGBToHSV( const double rgb[ 3 ], double& hue, double& sat, double& val );
/// The sRGB transfer function (linear -> encoded), both ways.
double EncodeSRGB( double linear );
double DecodeSRGB( double encoded );

/// The transmittance of a layer at the k-th sample: 10^( -l sum eps_i c_i ).
/// `depthCm` in cm; `conc` in M, one per species in `species`.
double Transmittance( const std::vector< Species >& species, const std::vector< double >& conc, double depthCm, int k );

/// The REFERENCE colour of a layer over a lightbox, linear sRGB, exposure 0:
/// the lightbox normalised to Y = 1 through T.
void LayerColour( Lightbox box, const std::vector< Species >& species, const std::vector< double >& conc, double depthCm,
                  double rgb[ 3 ] );
/// The REFERENCE filter matrix of a layer for the Over: linear clip RGB in,
/// filtered RGB out; the identity when every concentration is 0.
void LayerMatrix( const std::vector< Species >& species, const std::vector< double >& conc, double depthCm, double m[ 9 ] );

//---------------------------------------------------------------------------
// What the GPU is handed.
//---------------------------------------------------------------------------
/// The epsilon table: S_COUNT rows x kLambdaCount columns, M^-1 cm^-1.
std::vector< float > EpsilonTable();
/// The per-sample weights of a lightbox: w_k such that the source's colour is
/// sum_k w_k T_k (linear sRGB; T = 1 everywhere gives the lightbox's white
/// at Y = 1). 3 floats per sample.
std::vector< float > LightboxWeights( Lightbox box );
/// The per-sample 3x3 weights of the Over's filter: M = I + sum_k W_k ( T_k - 1 ),
/// column-major 3x3 per sample, so that T = 1 is the identity EXACTLY.
std::vector< float > PrimaryWeights();

} // namespace honeydew::spectra
