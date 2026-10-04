#include "Spectra.h"

#include <algorithm>
#include <cmath>

namespace honeydew::spectra
{
namespace
{
struct Entry
{
	Species species;
	const char* name;
	Citation citation;
	std::vector< Gaussian > gaussians;
};

//---------------------------------------------------------------------------
// The spectra. Each first Gaussian is the cited peak; the rest shape the band.
// CHEMISTRY.md repeats every number with its source.
//---------------------------------------------------------------------------
const std::vector< Entry >& table()
{
	static const std::vector< Entry > t = {
		{ S_FERROIN, "ferroin",
		  { 510.0, 11100.0, false, "Fe(phen)3 2+ lambda_max 510 nm, eps 1.11e4 M^-1 cm^-1 (the classic analytical value); the width and the blue-side shoulder (440 nm, 4.5e3) are a fit to the band's known breadth (it absorbs from 400 to 550 nm)" },
		  { { 510.0, 11100.0, 36.0 }, { 440.0, 4500.0, 30.0 } } },
		{ S_FERRIIN, "ferriin",
		  { 600.0, 870.0, true,
		    "Fe(phen)3 3+: pale blue, lambda_max ~590-600 nm; eps STAND-IN 870 (from memory of BZ imaging work; a web summary's 7200 at 593 nm is inconsistent with a pale solution and was not verified)" },
		  { { 600.0, 870.0, 60.0 } } },
		{ S_RU2, "Ru(bpy)3 2+",
		  { 452.0, 14600.0, false, "Ru(bpy)3 2+ MLCT 452 nm, eps 1.46e4 (Kalyanasundaram 1982 and many since); width a fit" },
		  { { 452.0, 14600.0, 30.0 }, { 400.0, 5000.0, 22.0 } } },
		{ S_RU3, "Ru(bpy)3 3+",
		  { 670.0, 420.0, true,
		    "Ru(bpy)3 3+: a broad weak band at 670 nm (Faraday Discuss. 2019, 'much lower' than the 2+); eps STAND-IN 420 and a 420 nm band of 3000 STAND-IN (Kalyanasundaram 1982 from memory)" },
		  { { 670.0, 420.0, 55.0 }, { 420.0, 3000.0, 30.0 } } },
		{ S_CE4, "Ce(IV)",
		  { 320.0, 5580.0, true,
		    "Ce(IV) in H2SO4: 320 nm, eps 5580 (the ceric sulfate dosimeter's value, from memory: STAND-IN status); the visible tail is the fitted width" },
		  { { 320.0, 5580.0, 58.0 } } },
		{ S_I2, "iodine",
		  { 460.0, 746.0, false, "I2(aq) 460 nm, eps 746 (Awtrey & Connick 1951); width a fit" },
		  { { 460.0, 746.0, 45.0 } } },
		{ S_I3, "triiodide",
		  { 353.0, 26400.0, false, "I3- 353 nm eps 26400 and 288 nm eps 40000 (Awtrey & Connick 1951), and its weak 460 nm band, eps 975 (Awtrey & Connick 1951, from memory of their table: STAND-IN status); widths a fit" },
		  { { 353.0, 26400.0, 32.0 }, { 288.0, 40000.0, 20.0 }, { 460.0, 975.0, 45.0 } } },
		{ S_STARCH_I3, "starch-iodine",
		  { 620.0, 40000.0, true,
		    "amylose-polyiodide: lambda_max 600-620 nm (the preprints.org review of the iodine/iodide/starch complex); eps per bound I3- STAND-IN 4e4; width a fit" },
		  { { 620.0, 40000.0, 70.0 } } },
		{ S_CLO2, "chlorine dioxide",
		  { 359.0, 1250.0, false, "ClO2(aq) 359 nm, eps 1250 (standard analytical value); width a fit" },
		  { { 359.0, 1250.0, 40.0 } } },
		{ S_IC_BLUE, "indigo carmine (blue form)",
		  { 610.0, 9000.0, false, "indigo carmine 610 nm, eps 9.0e3 (PMC9369777's measurement; literature 8.1e3-1.17e4); width a fit" },
		  { { 610.0, 9000.0, 42.0 } } },
		{ S_IC_YELLOW, "indigo carmine (alkaline form)",
		  { 430.0, 5000.0, true,
		    "the yellow dianion above pKa ~12.2: isosbestic points at 494 and 656 nm are reported (PCCP 2012); its lambda_max and eps are STAND-INS (430 nm, 5e3)" },
		  { { 430.0, 5000.0, 45.0 } } },
		{ S_IC_SEMI, "indigo carmine semiquinone",
		  { 500.0, 20000.0, true,
		    "the red one-electron intermediate (J. Chem. Soc. Faraday Trans. 1994, 90, 2525 has its stopped-flow spectrum, not reachable here): lambda_max 500 nm and eps 2e4 are STAND-INS" },
		  { { 500.0, 20000.0, 40.0 } } },
		{ S_IC_LEUCO, "leuco indigo carmine",
		  { 410.0, 6000.0, true,
		    "leuco-indigo absorbs at 410 nm in alkali (several sources); applied to indigo carmine, eps STAND-IN 6e3" },
		  { { 410.0, 6000.0, 40.0 } } },
		{ S_MB, "methylene blue",
		  { 664.0, 74000.0, false,
		    "methylene blue monomer 664 nm, eps 7.40e4 (omlc.org's tabulated spectrum, Prahl, 10 uM in water; others 8.2e4-9.5e4); the 612 nm shoulder is in the table too" },
		  { { 664.0, 66000.0, 16.0 }, { 612.0, 32000.0, 34.0 } } },
		{ S_MB_DIMER, "methylene blue dimer",
		  { 605.0, 132000.0, true,
		    "the dimer band at 605 nm (Bergmann & O'Konski 1963); eps per dimer 1.32e5 as tabulated on Wikipedia; K_dimer 'of order 1e3 M^-1' -- 2e3 used: STAND-IN order of magnitude" },
		  { { 605.0, 132000.0, 22.0 } } },
		{ S_RESAZURIN, "resazurin",
		  { 600.0, 60000.0, true,
		    "resazurin 600-602 nm in alkali; eps STAND-IN 6e4 (the alamarBlue assay table gives 1.17e5; other reports less)" },
		  { { 600.0, 60000.0, 28.0 } } },
		{ S_RESORUFIN, "resorufin",
		  { 571.0, 73000.0, false,
		    "resorufin anion 571-572 nm, eps 7.3e4 (Sigma product sheet, pH 8; reports range 5.4e4-9.2e4); the width and the 540 nm shoulder are a fit to the anion band's shape" },
		  { { 571.0, 73000.0, 20.0 }, { 540.0, 30000.0, 28.0 }, { 480.0, 26000.0, 45.0 } } },
		{ S_MNO4, "permanganate",
		  { 526.0, 2400.0, false, "MnO4- 526 nm eps 2.40e3 and 546 nm eps 2.38e3 (spectrophotometric analyses of KMnO4), with the 507 and 566 nm shoulders of its vibronic progression: four Gaussians whose SUM is 2414 at 526 and 2389 at 546; widths a fit" },
		  { { 526.0, 2250.0, 8.0 }, { 546.0, 2250.0, 8.0 }, { 507.0, 1200.0, 8.0 }, { 566.0, 900.0, 8.0 } } },
		{ S_MNO4_2, "manganate",
		  { 606.0, 1700.0, true, "MnO4 2- 606 and 439 nm (the manganate bands reported in alkaline permanganate kinetics); eps STAND-INS 1700 and 1400" },
		  { { 606.0, 1700.0, 30.0 }, { 439.0, 1400.0, 28.0 } } },
		{ S_MNO2, "colloidal MnO2",
		  { 380.0, 5000.0, true,
		    "soluble colloidal MnO2 (Pérez-Benito and co-workers, followed at 418 nm): a broad extinction rising into the UV; modelled as one Gaussian at 380 nm, eps per Mn STAND-IN 5000 (extinction, scattering included), sigma 110" },
		  { { 380.0, 5000.0, 110.0 } } },
	};
	return t;
}

const Entry& entry( Species s )
{
	for( const Entry& e : table() )
		if( e.species == s )
			return e;
	return table().front();
}

double lambdaOf( int k )
{
	return cie::kLambda0 + cie::kLambdaStep * k;
}

double gauss( double nm, double centre, double sigma )
{
	const double z = ( nm - centre ) / sigma;
	return std::exp( -0.5 * z * z );
}

void xyzOf( const double* power, double xyz[ 3 ] )
{
	xyz[ 0 ] = xyz[ 1 ] = xyz[ 2 ] = 0.0;
	for( int k = 0; k < cie::kLambdaCount; ++k )
		for( int c = 0; c < 3; ++c )
			xyz[ c ] += power[ k ] * cie::kXyzBar[ k ][ c ];
}
} // namespace

const char* SpeciesName( Species s )
{
	return entry( s ).name;
}
const Citation& CitationOf( Species s )
{
	return entry( s ).citation;
}
const std::vector< Gaussian >& GaussiansOf( Species s )
{
	return entry( s ).gaussians;
}
double Epsilon( Species s, double nm )
{
	double e = 0.0;
	for( const Gaussian& g : entry( s ).gaussians )
		e += g.eps * gauss( nm, g.nm, g.sigma );
	return e;
}

const std::vector< NamedSolution >& NamedSolutions()
{
	//A path and concentration that absorb about one absorbance unit at the
	//peak, so the hue is the solution's and not the lightbox's.
	static const std::vector< NamedSolution > s = {
		{ S_FERROIN, 1.0e-3, 1.0, "red", 345.0, 25.0 },
		{ S_FERRIIN, 1.0e-2, 1.0, "blue", 190.0, 250.0 },
		{ S_RU2, 7.0e-4, 1.0, "orange", 20.0, 58.0 },
		{ S_RU3, 2.0e-2, 1.0, "green", 70.0, 170.0 },
		{ S_CE4, 1.0e-2, 1.0, "yellow", 40.0, 70.0 },
		{ S_I2, 1.0e-2, 1.0, "orange", 15.0, 50.0 },
		{ S_I3, 5.0e-3, 1.0, "yellow", 35.0, 70.0 },
		{ S_STARCH_I3, 2.0e-4, 1.0, "blue", 200.0, 260.0 },
		{ S_CLO2, 2.0e-2, 1.0, "yellow", 40.0, 75.0 },
		{ S_IC_BLUE, 1.0e-3, 1.0, "blue", 195.0, 250.0 },
		{ S_IC_YELLOW, 2.0e-3, 1.0, "yellow", 40.0, 70.0 },
		{ S_IC_SEMI, 1.0e-3, 1.0, "red", 300.0, 30.0 },
		{ S_IC_LEUCO, 2.0e-3, 1.0, "yellow", 40.0, 75.0 },
		{ S_MB, 1.5e-4, 1.0, "blue", 185.0, 250.0 },
		{ S_MB_DIMER, 8.0e-5, 1.0, "blue", 185.0, 255.0 },
		{ S_RESAZURIN, 1.5e-4, 1.0, "blue", 195.0, 260.0 },
		{ S_RESORUFIN, 3.0e-5, 7.0, "pink", 300.0, 345.0 },
		{ S_MNO4, 4.0e-3, 1.0, "purple", 265.0, 330.0 },
		{ S_MNO4_2, 6.0e-3, 1.0, "green", 75.0, 165.0 },
		{ S_MNO2, 2.0e-3, 1.0, "yellow", 25.0, 60.0 },
	};
	return s;
}

//---------------------------------------------------------------------------
double LightboxPower( Lightbox box, int k )
{
	const double nm = lambdaOf( k );
	switch( box )
	{
	case Lightbox::D65: return cie::kD65[ k ];
	case Lightbox::LED5000K:
		//A phosphor-converted white LED, modelled: a 450 nm pump (sigma 10)
		//and a broad phosphor band at 570 nm (sigma 60), weights chosen so the
		//white sits near 5000 K on the daylight locus (CHEMISTRY.md).
		return 1.0 * gauss( nm, 450.0, 10.0 ) + 1.55 * gauss( nm, 570.0, 60.0 );
	case Lightbox::WarmWhite:
		//The same model at 2700 K: a weaker pump and a redder phosphor.
		return 0.35 * gauss( nm, 452.0, 10.0 ) + 1.7 * gauss( nm, 600.0, 65.0 );
	default: return 1.0;
	}
}

double PrimaryPower( int primary, int k )
{
	const double nm = lambdaOf( k );
	switch( primary )
	{
	case 0: return gauss( nm, 630.0, 12.0 );
	case 1: return gauss( nm, 530.0, 18.0 );
	default: return gauss( nm, 460.0, 11.0 );
	}
}

//---------------------------------------------------------------------------
void XYZToLinearSRGB( const double xyz[ 3 ], double rgb[ 3 ] )
{
	rgb[ 0 ] = 3.2406 * xyz[ 0 ] - 1.5372 * xyz[ 1 ] - 0.4986 * xyz[ 2 ];
	rgb[ 1 ] = -0.9689 * xyz[ 0 ] + 1.8758 * xyz[ 1 ] + 0.0415 * xyz[ 2 ];
	rgb[ 2 ] = 0.0557 * xyz[ 0 ] - 0.2040 * xyz[ 1 ] + 1.0570 * xyz[ 2 ];
}

void RGBToHSV( const double rgb[ 3 ], double& hue, double& sat, double& val )
{
	const double r = std::max( rgb[ 0 ], 0.0 ), g = std::max( rgb[ 1 ], 0.0 ), b = std::max( rgb[ 2 ], 0.0 );
	const double mx = std::max( { r, g, b } ), mn = std::min( { r, g, b } ), d = mx - mn;
	val = mx;
	sat = mx > 0.0 ? d / mx : 0.0;
	if( d <= 0.0 )
	{
		hue = 0.0;
		return;
	}
	if( mx == r )
		hue = 60.0 * std::fmod( ( g - b ) / d + 6.0, 6.0 );
	else if( mx == g )
		hue = 60.0 * ( ( b - r ) / d + 2.0 );
	else
		hue = 60.0 * ( ( r - g ) / d + 4.0 );
}

double EncodeSRGB( double v )
{
	v = std::clamp( v, 0.0, 1.0 );
	return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow( v, 1.0 / 2.4 ) - 0.055;
}
double DecodeSRGB( double v )
{
	v = std::clamp( v, 0.0, 1.0 );
	return v <= 0.04045 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 );
}

double Transmittance( const std::vector< Species >& species, const std::vector< double >& conc, double depthCm, int k )
{
	double absorbance = 0.0;
	const double nm   = lambdaOf( k );
	for( size_t i = 0; i < species.size() && i < conc.size(); ++i )
		absorbance += Epsilon( species[ i ], nm ) * std::max( conc[ i ], 0.0 );
	return std::pow( 10.0, -absorbance * depthCm );
}

void LayerColour( Lightbox box, const std::vector< Species >& species, const std::vector< double >& conc, double depthCm,
                  double rgb[ 3 ] )
{
	double white[ cie::kLambdaCount ], through[ cie::kLambdaCount ];
	for( int k = 0; k < cie::kLambdaCount; ++k )
	{
		white[ k ]   = LightboxPower( box, k );
		through[ k ] = white[ k ] * Transmittance( species, conc, depthCm, k );
	}
	double xyzWhite[ 3 ], xyz[ 3 ];
	xyzOf( white, xyzWhite );
	xyzOf( through, xyz );
	for( int c = 0; c < 3; ++c )
		xyz[ c ] /= xyzWhite[ 1 ];
	XYZToLinearSRGB( xyz, rgb );
}

namespace
{
/// The 3x3 that takes a (linear) primary weight triple to XYZ at T = 1, and
/// its inverse composed with XYZ -> sRGB: so that the filter matrix is the
/// identity for an empty dish.
void primaryBasis( double toXYZ[ 9 ], double normaliser[ 9 ] )
{
	//toXYZ[ c * 3 + p ] = XYZ_c of primary p at T = 1
	for( int p = 0; p < 3; ++p )
	{
		double power[ cie::kLambdaCount ], xyz[ 3 ];
		for( int k = 0; k < cie::kLambdaCount; ++k )
			power[ k ] = PrimaryPower( p, k );
		xyzOf( power, xyz );
		for( int c = 0; c < 3; ++c )
			toXYZ[ c * 3 + p ] = xyz[ c ];
	}
	//S = sRGB( toXYZ ): the sRGB each primary makes at T = 1. normaliser = S^-1.
	double S[ 9 ];
	for( int p = 0; p < 3; ++p )
	{
		const double xyz[ 3 ] = { toXYZ[ 0 * 3 + p ], toXYZ[ 1 * 3 + p ], toXYZ[ 2 * 3 + p ] };
		double rgb[ 3 ];
		XYZToLinearSRGB( xyz, rgb );
		for( int c = 0; c < 3; ++c )
			S[ c * 3 + p ] = rgb[ c ];
	}
	//3x3 inverse.
	const double a = S[ 0 ], b = S[ 1 ], c = S[ 2 ], d = S[ 3 ], e = S[ 4 ], f = S[ 5 ], g = S[ 6 ], h = S[ 7 ], i = S[ 8 ];
	const double det = a * ( e * i - f * h ) - b * ( d * i - f * g ) + c * ( d * h - e * g );
	const double inv[ 9 ] = { ( e * i - f * h ) / det, -( b * i - c * h ) / det, ( b * f - c * e ) / det,
		                      -( d * i - f * g ) / det, ( a * i - c * g ) / det, -( a * f - c * d ) / det,
		                      ( d * h - e * g ) / det, -( a * h - b * g ) / det, ( a * e - b * d ) / det };
	for( int n = 0; n < 9; ++n )
		normaliser[ n ] = inv[ n ];
}

/// W_k: the 3x3 (row-major) such that the Over's matrix is I + sum_k W_k ( T_k - 1 ).
void perSampleMatrices( std::vector< double >& W )
{
	double toXYZ[ 9 ], N[ 9 ];
	primaryBasis( toXYZ, N );
	W.assign( static_cast< size_t >( cie::kLambdaCount ) * 9, 0.0 );
	for( int k = 0; k < cie::kLambdaCount; ++k )
	{
		//M_k[ c ][ p ] = sRGB_c( CMF_k ) * primary_p( k ): XYZ_c contribution of primary p at sample k
		double rgbOfSample[ 3 ];
		const double xyz[ 3 ] = { cie::kXyzBar[ k ][ 0 ], cie::kXyzBar[ k ][ 1 ], cie::kXyzBar[ k ][ 2 ] };
		XYZToLinearSRGB( xyz, rgbOfSample );
		double Mk[ 9 ];
		for( int c = 0; c < 3; ++c )
			for( int p = 0; p < 3; ++p )
				Mk[ c * 3 + p ] = rgbOfSample[ c ] * PrimaryPower( p, k );
		//W_k = N * Mk
		for( int r = 0; r < 3; ++r )
			for( int cc = 0; cc < 3; ++cc )
			{
				double s = 0.0;
				for( int m = 0; m < 3; ++m )
					s += N[ r * 3 + m ] * Mk[ m * 3 + cc ];
				W[ static_cast< size_t >( k ) * 9 + r * 3 + cc ] = s;
			}
	}
}
} // namespace

void LayerMatrix( const std::vector< Species >& species, const std::vector< double >& conc, double depthCm, double m[ 9 ] )
{
	std::vector< double > W;
	perSampleMatrices( W );
	for( int n = 0; n < 9; ++n )
		m[ n ] = n % 4 == 0 ? 1.0 : 0.0;
	for( int k = 0; k < cie::kLambdaCount; ++k )
	{
		const double t = Transmittance( species, conc, depthCm, k ) - 1.0;
		for( int n = 0; n < 9; ++n )
			m[ n ] += W[ static_cast< size_t >( k ) * 9 + n ] * t;
	}
}

//---------------------------------------------------------------------------
std::vector< float > EpsilonTable()
{
	std::vector< float > t( static_cast< size_t >( S_COUNT ) * cie::kLambdaCount );
	for( int s = 0; s < S_COUNT; ++s )
		for( int k = 0; k < cie::kLambdaCount; ++k )
			t[ static_cast< size_t >( s ) * cie::kLambdaCount + k ] = static_cast< float >( Epsilon( static_cast< Species >( s ), lambdaOf( k ) ) );
	return t;
}

std::vector< float > LightboxWeights( Lightbox box )
{
	double white[ cie::kLambdaCount ], xyzWhite[ 3 ];
	for( int k = 0; k < cie::kLambdaCount; ++k )
		white[ k ] = LightboxPower( box, k );
	xyzOf( white, xyzWhite );
	std::vector< float > w( static_cast< size_t >( cie::kLambdaCount ) * 3 );
	for( int k = 0; k < cie::kLambdaCount; ++k )
	{
		const double xyz[ 3 ] = { cie::kXyzBar[ k ][ 0 ] * white[ k ] / xyzWhite[ 1 ], cie::kXyzBar[ k ][ 1 ] * white[ k ] / xyzWhite[ 1 ],
			                      cie::kXyzBar[ k ][ 2 ] * white[ k ] / xyzWhite[ 1 ] };
		double rgb[ 3 ];
		XYZToLinearSRGB( xyz, rgb );
		for( int c = 0; c < 3; ++c )
			w[ static_cast< size_t >( k ) * 3 + c ] = static_cast< float >( rgb[ c ] );
	}
	return w;
}

std::vector< float > PrimaryWeights()
{
	std::vector< double > W;
	perSampleMatrices( W );
	//Column-major per sample, for a GLSL mat3.
	std::vector< float > out( W.size() );
	for( int k = 0; k < cie::kLambdaCount; ++k )
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 3; ++c )
				out[ static_cast< size_t >( k ) * 9 + c * 3 + r ] = static_cast< float >( W[ static_cast< size_t >( k ) * 9 + r * 3 + c ] );
	return out;
}

} // namespace honeydew::spectra
