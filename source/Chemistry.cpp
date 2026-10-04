#include "Chemistry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace honeydew::chem
{
const Oregonator kOregonator;
const LengyelEpstein kLE;
const ClockKinetics kClock;
const DyeKinetics kDye;
const ChameleonKinetics kChameleon;
const BRKinetics kBR;

namespace
{
constexpr double kPi = 3.14159265358979323846;
/// A batch dish of BZ runs for about an hour (STAND-IN: the Oregonator's
/// lumped step 5 would spend the malonic acid in minutes, which a real dish
/// does not; the fuel is spent at this rate instead, per unit of v).
constexpr double kBZBatchSeconds = 3600.0;
/// The CSTR feed used for Flow in every reaction but BR (whose Binous
/// recipe has its own): a residence of 300 s (STAND-IN).
constexpr double kFlowK0 = 1.0 / 300.0;
} // namespace

//---------------------------------------------------------------------------
Recipe BaseRecipe( Reaction r )
{
	Recipe x;
	switch( r )
	{
	case Reaction::BZ:
		//A thin-layer ferroin dish: 0.3 M bromate, 0.3 M H+, 0.1 M malonic
		//acid, 1 mM catalyst (the order of the Winfree / Zaikin-Zhabotinsky
		//recipes; CHEMISTRY.md).
		x.oxidant   = 0.3;
		x.acidBase  = 0.3;
		x.reductant = 0.1;
		x.indicator = 1.0e-3;
		break;
	case Reaction::BriggsRauscher:
		//A demonstration recipe (iodate 0.067, H+ 0.08, malonic acid 0.05,
		//Mn 0.02, H2O2 1.3 M: Shakhashiri's proportions), which sits inside
		//the transcribed De Kepper-Epstein model's batch-oscillatory region
		//(CHEMISTRY.md: period 139 s, iodine peaks 1.7 mM, five oscillations
		//then it runs down); starch 1 g/L as 5e-4 M of helix sites (a STAND-IN
		//unit, see Spectra).
		x.oxidant   = 0.067;
		x.acidBase  = 0.08;
		x.reductant = 0.05;
		x.indicator = 5.0e-4;
		break;
	case Reaction::IodineClock:
		x.oxidant   = 0.1;  //H2O2
		x.acidBase  = 0.05; //H+
		x.reductant = 0.005;//thiosulfate
		x.indicator = 5.0e-4;//starch sites (M of helix sites binding one I3- each: a STAND-IN unit for 1 g/L)
		break;
	case Reaction::CDIMA:
		//The gel-reactor concentrations of arXiv:2504.02530 Table 3 (ClO2
		//2e-3, PVA/starch 1.5e-3 as sites, H+ at acetic acid's pH ~ 3), with
		//the malonic acid raised from its 2.25e-3 to 3.5e-3 M: in the reduced
		//model the table's value sits 10% OUTSIDE the Turing region (the
		//condition on a, b, d fails; CHEMISTRY.md), 3.5e-3 is inside it.
		x.oxidant   = 2.0e-3;
		x.acidBase  = 1.0e-3;
		x.reductant = 3.5e-3;
		x.indicator = 1.5e-3;
		break;
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
		//Pons 2000's glucose and (for the blue bottle) dye, with four times
		//their [OH-] so that the 1/4x slider reaches their 0.020 M and 4x the
		//classical demonstration's ~0.3 M. Oxidant 1x = air saturation. The
		//traffic light's indigo carmine (1e-3 M, ~0.47 g/L) and the valentine's
		//resazurin (1e-4 M) are demonstration strengths, chosen to absorb
		//about half an absorbance unit in a 1.5 mm layer.
		x.oxidant   = kDye.O2sat;
		x.acidBase  = 0.08;
		x.reductant = 0.054;
		x.indicator = r == Reaction::BlueBottle ? 4.6e-5 : r == Reaction::TrafficLight ? 1.0e-3 : 1.0e-4;
		break;
	case Reaction::Chameleon:
		//A demonstration recipe: 0.25 M NaOH, 0.05 M glucose, 1.3 mM KMnO4
		//per dose and in the dish at the start.
		x.oxidant   = 1.3e-3;
		x.acidBase  = 0.25;
		x.reductant = 0.05;
		x.indicator = 1.3e-3;
		break;
	default: break;
	}
	return x;
}

Recipe ScaledRecipe( Reaction r, double mOxidant, double mAcidBase, double mReductant, double mIndicator )
{
	Recipe x = BaseRecipe( r );
	x.oxidant *= mOxidant;
	x.acidBase *= mAcidBase;
	x.reductant *= mReductant;
	x.indicator *= mIndicator;
	return x;
}

//---------------------------------------------------------------------------
BZModel MakeBZ( const Recipe& recipe, double f )
{
	BZModel m;
	const Oregonator& k = kOregonator;
	const double A = recipe.oxidant, H = recipe.acidBase, B = recipe.reductant;
	m.f    = f;
	m.Ctot = recipe.indicator;
	m.Du   = k.DX * kCm2PerS_to_Mm2PerS;
	m.Dv   = k.DZ * kCm2PerS_to_Mm2PerS;
	if( A <= 0.0 || H <= 0.0 || B <= 0.0 )
	{
		//A reagent missing: nothing happens. eps and q keep harmless values,
		//the time scale is infinite.
		m.eps      = 1.0;
		m.epsPrime = 1.0;
		m.q        = 2.0 * k.k1 * k.k3 / ( k.k2 * k.k4 );
		m.T0       = 1.0e30;
		m.X0 = m.Z0 = 0.0;
		return m;
	}
	const double k5  = k.kc * B;
	const double k4AH = k.k4 * A * H;
	m.eps      = k5 / k4AH;
	m.epsPrime = 2.0 * k.k3 * k5 / ( k.k2 * k4AH * H );
	m.q        = 2.0 * k.k1 * k.k3 / ( k.k2 * k.k4 );
	m.T0       = 1.0 / k5;
	m.X0       = k4AH / ( 2.0 * k.k3 );
	m.Z0       = k4AH * k4AH / ( k.k3 * k5 );
	m.frontSpeedMmPerS = 2.0 * std::sqrt( k.DX * k4AH ) * 10.0;
	return m;
}

double FieldNoyesSpeedMmPerS( double H, double A )
{
	//v = 0.04 cm s^-1 M^-1 ( [H+][BrO3-] )^1/2 (Field & Noyes 1974, as quoted
	//by Tyson; the "cm^2 sec^-1 M^-1" of some transcriptions is a misprint of
	//the unit: the right-hand side is a speed).
	return 0.04 * std::sqrt( std::max( 0.0, H * A ) ) * 10.0;
}

void TysonFifeRhs( const BZModel& m, double phi, double u, double v, double& du, double& dv )
{
	du = ( u - u * u - ( m.f * v + phi ) * ( u - m.q ) / ( u + m.q ) ) / m.eps;
	dv = u - v;
}

void OregonatorRhs( const BZModel& m, double phi, const double* s, double* d )
{
	const double x = s[ 0 ], y = s[ 1 ], z = s[ 2 ];
	d[ 0 ] = ( m.q * y - x * y + x * ( 1.0 - x ) ) / m.eps;
	d[ 1 ] = ( -m.q * y - x * y + m.f * z + phi ) / m.epsPrime;
	d[ 2 ] = x - z;
}

void BZRestState( const BZModel& m, double xyz[ 3 ] )
{
	//x = z and y = f z / ( q + x ): x^2 - ( 1 - q - f ) x - q ( 1 + f ) = 0.
	const double bq = 1.0 - m.q - m.f;
	const double x  = 0.5 * ( bq + std::sqrt( bq * bq + 4.0 * m.q * ( 1.0 + m.f ) ) );
	xyz[ 0 ]        = x;
	xyz[ 1 ]        = m.f * x / ( m.q + x );
	xyz[ 2 ]        = x;
}

double BZPeakZ( const BZModel& m )
{
	struct Key
	{
		double eps, epsPrime, q, f, peak;
	};
	static std::vector< Key > cache;
	for( const Key& k : cache )
		if( k.eps == m.eps && k.epsPrime == m.epsPrime && k.q == m.q && k.f == m.f )
			return k.peak;
	double y[ 3 ];
	BZRestState( m, y );
	y[ 0 ] *= 1.01;//off the rest state, so an oscillatory medium starts
	double peak = 0.0, lastZ = y[ 2 ], lastDz = 0.0, lastPeak = 0.0;
	int peaks = 0;
	//Twenty time units: several periods at any recipe that oscillates.
	IntegrateStiff( 3, [ & ]( const double* s, double* d ) { OregonatorRhs( m, 0.0, s, d ); }, y, 20.0, 1e-7, 1e-12,
	                [ & ]( double, const double* s ) {
		                const double dz = s[ 2 ] - lastZ;
		                if( lastDz > 0.0 && dz <= 0.0 && s[ 2 ] > 0.02 )
		                {
			                lastPeak = s[ 2 ];
			                ++peaks;
		                }
		                lastDz = dz;
		                lastZ  = s[ 2 ];
	                }, 0.01 );
	peak = peaks >= 2 ? lastPeak : 0.0;
	if( cache.size() > 64 )
		cache.erase( cache.begin() );
	cache.push_back( { m.eps, m.epsPrime, m.q, m.f, peak } );
	return peak;
}

//---------------------------------------------------------------------------
namespace
{
/// det( J - k^2 D ) < 0 for some k^2: the Turing test of the LE steady state
/// at the given sigma. Fills kc (per x') when unstable.
bool leTuring( double a, double b, double sigma, double d, double& kc )
{
	const double u0 = a / 5.0, v0 = 1.0 + u0 * u0;
	const double den = ( 1.0 + u0 * u0 ) * ( 1.0 + u0 * u0 );
	const double g   = ( 1.0 - u0 * u0 ) / den;//d/du of u/(1+u^2)
	const double J11 = ( -1.0 - 4.0 * v0 * g ) / sigma;
	const double J12 = ( -4.0 * u0 / ( 1.0 + u0 * u0 ) ) / sigma;
	const double J21 = b * ( 1.0 - v0 * g );
	const double J22 = -b * u0 / ( 1.0 + u0 * u0 );
	const double det = J11 * J22 - J12 * J21;
	const double D1 = 1.0 / sigma, D2 = d;
	//h(k2) = D1 D2 k2^2 - ( D2 J11 + D1 J22 ) k2 + det
	const double lin = D2 * J11 + D1 * J22;
	kc               = 0.0;
	if( lin <= 0.0 )
		return false;
	const double disc = lin * lin - 4.0 * D1 * D2 * det;
	if( disc <= 0.0 )
		return false;
	kc = std::sqrt( lin / ( 2.0 * D1 * D2 ) );
	return true;
}
} // namespace

LEModel MakeLE( const Recipe& recipe )
{
	LEModel m;
	const LengyelEpstein& k = kLE;
	const double ClO2 = recipe.oxidant, MA = recipe.reductant, S = recipe.indicator, I2 = k.I2;
	const double sa = std::sqrt( k.alpha );
	if( ClO2 <= 0.0 )
	{
		m.tScale = 1.0e30;
		m.xScale = 1.0;
		return m;
	}
	m.a      = k.k1a * MA * I2 / ( sa * k.k2 * ClO2 * ( k.k1b + I2 ) );
	m.b      = k.k3b * I2 / ( sa * k.k2 * ClO2 );
	m.sigma  = 1.0 + k.K * S * I2;
	m.d      = k.DClO2m / k.DI;
	m.tScale = 1.0 / ( k.k2 * ClO2 );
	m.xScale = std::sqrt( k.DI / ( k.k2 * ClO2 ) ) * 10.0;
	m.Du     = k.DI * kCm2PerS_to_Mm2PerS / m.sigma;
	m.Dv     = k.DClO2m * kCm2PerS_to_Mm2PerS;
	m.u0     = m.a / 5.0;
	m.v0     = 1.0 + m.u0 * m.u0;
	m.turing = leTuring( m.a, m.b, m.sigma, m.d, m.kc );
	m.lambdaMm = m.kc > 0.0 ? 2.0 * kPi / m.kc * m.xScale : 0.0;
	//The sigma where the instability sets in, by bisection: a and b do not
	//depend on sigma.
	double lo = 1.0, hi = 1.0e5, kk;
	if( !leTuring( m.a, m.b, hi, m.d, kk ) )
		m.sigmaCritical = -1.0;//never
	else if( leTuring( m.a, m.b, lo, m.d, kk ) )
		m.sigmaCritical = lo;//always
	else
	{
		for( int i = 0; i < 100; ++i )
		{
			const double mid = std::sqrt( lo * hi );
			if( leTuring( m.a, m.b, mid, m.d, kk ) )
				hi = mid;
			else
				lo = mid;
		}
		m.sigmaCritical = hi;
	}
	return m;
}

void LERhs( const LEModel& m, double w, double u, double v, double& du, double& dv )
{
	const double r = u * v / ( 1.0 + u * u );
	du             = ( m.a - u - 4.0 * r - w ) / m.sigma;
	dv             = m.b * ( u - r + w );
}

//---------------------------------------------------------------------------
double ClockRateConstant( double H )
{
	return kClock.k1 + kClock.k2 * H;
}

double ClockSwitchTime( double H0, double I0, double H, double S0 )
{
	if( S0 <= 0.0 )
		return 0.0;
	if( H0 <= 0.0 || I0 <= 0.0 || S0 >= 2.0 * H0 )
		return -1.0;
	return -std::log( 1.0 - S0 / ( 2.0 * H0 ) ) / ( ClockRateConstant( H ) * I0 );
}

double ClockDoseForSwitch( double tSwitch, double H0, double I0, double H )
{
	if( tSwitch <= 0.0 || H0 <= 0.0 || I0 <= 0.0 )
		return 0.0;
	return 2.0 * H0 * ( 1.0 - std::exp( -ClockRateConstant( H ) * I0 * tSwitch ) );
}

void IodineSpeciation( double T, double F, double K, double& I2, double& I3, double& iodideFree )
{
	//I3 = K ( T - I3 )( F - I3 ):  K I3^2 - ( 1 + K T + K F ) I3 + K T F = 0, the smaller root.
	T = std::max( T, 0.0 );
	F = std::max( F, 0.0 );
	if( T <= 0.0 || F <= 0.0 || K <= 0.0 )
	{
		I2         = T;
		I3         = 0.0;
		iodideFree = F;
		return;
	}
	const double bq = 1.0 + K * ( T + F );
	const double disc = std::sqrt( std::max( 0.0, bq * bq - 4.0 * K * K * T * F ) );
	//The numerically stable form of the smaller root.
	I3         = 2.0 * K * T * F / ( bq + disc );
	I3         = std::clamp( I3, 0.0, std::min( T, F ) );
	I2         = T - I3;
	iodideFree = F - I3;
}

//---------------------------------------------------------------------------
double DyeBlueDuration( double O2, double OH, double GL, double dyeTotal )
{
	const double rate = 0.5 * kDye.k2 * OH * GL * dyeTotal;
	return rate > 0.0 ? O2 / rate : -1.0;
}

double DyeOxygenForDuration( double tFade, double OH, double GL, double dyeTotal )
{
	return std::max( 0.0, tFade ) * 0.5 * kDye.k2 * OH * GL * dyeTotal;
}

double ChameleonGreenPeakTime( double OH, double GL )
{
	const double alpha = kChameleon.kA * OH * GL, beta = kChameleon.kB * OH * GL;
	if( alpha <= 0.0 || beta <= 0.0 )
		return -1.0;
	if( std::fabs( alpha - beta ) < 1e-12 * alpha )
		return 1.0 / alpha;
	return std::log( beta / alpha ) / ( beta - alpha );
}

//---------------------------------------------------------------------------
void BRRhs( const BRKinetics& k, double H, double k0, const double* feed, const double* y, double* dy )
{
	double c[ kBRSpecies ];
	for( int i = 0; i < kBRSpecies; ++i )
		c[ i ] = std::max( y[ i ], 0.0 );
	const double A = c[ BR_I ], B = c[ BR_I2 ], C = c[ BR_IO3 ], D = c[ BR_HOI ], E = c[ BR_HIO2 ], F = c[ BR_IO2 ],
	             G = c[ BR_MN3 ], I = c[ BR_HO2 ], J = c[ BR_MA ], K = c[ BR_H2O2 ];
	const double Mn2 = std::max( k.mnTotal - G, 0.0 );
	const double v1 = k.r1 * H * H * A * C, v2 = k.r2 * H * A * E, v3 = k.r3 * H * A * D, vm3 = k.rm3 * B,
	             v4 = k.r4 * C * E * H, vm4 = k.rm4 * F * F, v5 = k.r5 * E * E, v6 = k.r6 * F * Mn2, vm6 = k.rm6 * E * G,
	             v7 = k.r7 * G * K, v8 = k.r8 * I * I, v9 = k.r9 * B * J / ( 1.0 + k.c9 * B ), v10 = k.r10 * D * K;
	dy[ BR_I ]    = -v1 - v2 - v3 + vm3 + v9 + v10;
	dy[ BR_I2 ]   = v3 - vm3 - v9;
	dy[ BR_IO3 ]  = -v1 - v4 + vm4 + v5;
	dy[ BR_HOI ]  = v1 + 2.0 * v2 - v3 + vm3 + v5 - v10;
	dy[ BR_HIO2 ] = v1 - v2 - v4 + vm4 - 2.0 * v5 + v6 - vm6;
	dy[ BR_IO2 ]  = 2.0 * v4 - 2.0 * vm4 - v6 + vm6;
	dy[ BR_MN3 ]  = v6 - vm6 - v7;
	dy[ BR_HO2 ]  = v7 - 2.0 * v8;
	dy[ BR_MA ]   = -v9;
	dy[ BR_H2O2 ] = -v7 + v8 - v10;
	if( k0 > 0.0 && feed )
		for( int i = 0; i < kBRSpecies; ++i )
			dy[ i ] += k0 * ( feed[ i ] - y[ i ] );
}

void BRInitialState( const Recipe& recipe, double* y, double& H, double& H2O2 )
{
	for( int i = 0; i < kBRSpecies; ++i )
		y[ i ] = 0.0;
	//Traces of iodide and iodine start the autocatalysis (any real reagent
	//carries them); the exact values do not set the period.
	y[ BR_I ]    = 1.0e-7;
	y[ BR_I2 ]   = 1.0e-7;
	y[ BR_IO3 ]  = recipe.oxidant;
	y[ BR_HOI ]  = 1.0e-9;
	y[ BR_HIO2 ] = 1.0e-9;
	y[ BR_IO2 ]  = 1.0e-9;
	y[ BR_HO2 ]  = 1.0e-9;
	y[ BR_MA ]   = recipe.reductant;
	H            = recipe.acidBase;
	//The hydrogen peroxide follows the iodate slider (both are "the oxidant"
	//in a BR demonstration's solution A/C pairing) from the recipe's 1.3 M.
	const Recipe base = BaseRecipe( Reaction::BriggsRauscher );
	H2O2              = base.oxidant > 0.0 ? 1.3 * recipe.oxidant / base.oxidant : 0.0;
	y[ BR_H2O2 ]      = H2O2;
}

//---------------------------------------------------------------------------
Params MakeParams( Reaction r, const Recipe& recipe, Catalyst catalyst, Reactor reactor, double depthMm, double stir )
{
	Params p {};
	p[ P_REACTION ] = static_cast< float >( static_cast< int >( r ) );
	const bool batch = reactor == Reactor::Batch;
	p[ P_BATCH ]     = batch ? 1.0f : 0.0f;
	p[ P_FLOW_K0 ]   = batch ? 0.0f : static_cast< float >( r == Reaction::BriggsRauscher ? kBR.k0CSTR : kFlowK0 );
	switch( r )
	{
	case Reaction::BZ:
	{
		const BZModel m    = MakeBZ( recipe );
		p[ P_BZ_EPS ]      = static_cast< float >( m.eps );
		p[ P_BZ_Q ]        = static_cast< float >( m.q );
		p[ P_BZ_F ]        = static_cast< float >( m.f );
		p[ P_BZ_T0 ]       = static_cast< float >( std::min( m.T0, 1.0e30 ) );
		p[ P_BZ_DU ]       = static_cast< float >( m.Du );
		p[ P_BZ_DV ]       = static_cast< float >( m.Dv );
		p[ P_BZ_PHIMAX ]   = static_cast< float >( catalyst == Catalyst::Rubpy ? kPhiMax : 0.0 );
		p[ P_BZ_FUELRATE ] = static_cast< float >( batch ? 1.0 / ( 0.3 * kBZBatchSeconds ) : 0.0 );
		//Pacemakers: dust sites where the chemistry runs 30% faster (STAND-IN
		//heterogeneity model, AGENTS.md): they lead, and targets grow from them.
		p[ P_BZ_PACE ] = 0.3f;
		p[ P_BZ_CTOT ] = static_cast< float >( m.Ctot );
		p[ P_BZ_EPSP ] = static_cast< float >( m.epsPrime );
		p[ P_BZ_DY ]   = static_cast< float >( kOregonator.DY * kCm2PerS_to_Mm2PerS );
		//The picture's scale: the model's own peak z at this recipe (or 0.25,
		//a typical peak, where it does not oscillate so a drop still shows).
		const double peak = BZPeakZ( m );
		p[ P_BZ_ZREF ] = static_cast< float >( peak > 0.0 ? peak : 0.25 );
		break;
	}
	case Reaction::CDIMA:
	{
		const LEModel m  = MakeLE( recipe );
		p[ P_LE_A ]      = static_cast< float >( m.a );
		p[ P_LE_B ]      = static_cast< float >( m.b );
		p[ P_LE_SIGMA ]  = static_cast< float >( m.sigma );
		p[ P_LE_D ]      = static_cast< float >( m.d );
		p[ P_LE_TSCALE ] = static_cast< float >( std::min( m.tScale, 1.0e30 ) );
		p[ P_LE_DU ]     = static_cast< float >( kLE.DI * kCm2PerS_to_Mm2PerS );
		p[ P_LE_WMAX ]   = static_cast< float >( kWMaxFraction * m.a );
		break;
	}
	case Reaction::IodineClock:
		p[ P_CK_KP ]     = static_cast< float >( ClockRateConstant( recipe.acidBase ) );
		p[ P_CK_H2O2_0 ] = static_cast< float >( recipe.oxidant );
		p[ P_CK_I_0 ]    = static_cast< float >( kClock.iodide );
		p[ P_CK_S_0 ]    = static_cast< float >( recipe.reductant );
		p[ P_CK_DH2O2 ]  = static_cast< float >( kClock.DH2O2 * kCm2PerS_to_Mm2PerS );
		p[ P_CK_DI ]     = static_cast< float >( kClock.DI * kCm2PerS_to_Mm2PerS );
		p[ P_CK_DI2 ]    = static_cast< float >( kClock.DI2 * kCm2PerS_to_Mm2PerS );
		p[ P_CK_DS ]     = static_cast< float >( kClock.DS2O3 * kCm2PerS_to_Mm2PerS );
		break;
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
	{
		p[ P_DY_KOX ]   = static_cast< float >( kDye.kOx );
		p[ P_DY_K2 ]    = static_cast< float >( kDye.k2 );
		p[ P_DY_OH ]    = static_cast< float >( recipe.acidBase );
		p[ P_DY_O2SAT ] = static_cast< float >( recipe.oxidant );
		//k_L a of the layer: D / film / depth, times the stirring closure.
		const double kL  = kDye.DO2 / ( kDye.filmMm * 0.1 );//cm/s
		const double kLa = kL / std::max( depthMm * 0.1, 1e-3 ) * ( 1.0 + kDye.kStirAeration * stir * stir );
		p[ P_DY_KLA ]    = static_cast< float >( kLa );
		p[ P_DY_DO2 ]    = static_cast< float >( kDye.DO2 * kCm2PerS_to_Mm2PerS );
		p[ P_DY_DDYE ]   = static_cast< float >( kDye.Ddye * kCm2PerS_to_Mm2PerS );
		p[ P_DY_DGL ]    = static_cast< float >( kDye.DGL * kCm2PerS_to_Mm2PerS );
		p[ P_DY_CTOT ]   = static_cast< float >( recipe.indicator );
		p[ P_DY_KSQ ]    = static_cast< float >( kDye.kSemiquinone );
		p[ P_DY_PKA ]    = static_cast< float >( kDye.pKaIC );
		p[ P_DY_KRZ ]    = static_cast< float >( r == Reaction::Valentine ? kDye.kResazurin : 0.0 );
		p[ P_DY_GL0 ]    = static_cast< float >( recipe.reductant );
		p[ P_DY_TWOSTEP ] = r == Reaction::TrafficLight ? 1.0f : 0.0f;
		break;
	}
	case Reaction::Chameleon:
		p[ P_CH_KA ]    = static_cast< float >( kChameleon.kA );
		p[ P_CH_KB ]    = static_cast< float >( kChameleon.kB );
		p[ P_CH_OH ]    = static_cast< float >( recipe.acidBase );
		p[ P_CH_DMN ]   = static_cast< float >( kChameleon.DMn * kCm2PerS_to_Mm2PerS );
		p[ P_CH_DMNO2 ] = static_cast< float >( kChameleon.DMnO2 * kCm2PerS_to_Mm2PerS );
		p[ P_CH_DGL ]   = static_cast< float >( kChameleon.DGL * kCm2PerS_to_Mm2PerS );
		p[ P_CH_GL0 ]   = static_cast< float >( recipe.reductant );
		p[ P_CH_MN0 ]   = static_cast< float >( recipe.indicator );
		break;
	default: break;
	}
	return p;
}

void FreshState( Reaction r, const Params& p, double* a, double* b )
{
	for( int i = 0; i < 4; ++i )
		a[ i ] = b[ i ] = 0.0;
	switch( r )
	{
	case Reaction::BZ:
	{
		//The rest state of the three-variable model; B.x is the pacemaker
		//flag (the seed pass sets it), A.w the fuel.
		BZModel m;
		m.q        = p[ P_BZ_Q ];
		m.f        = p[ P_BZ_F ];
		double xyz[ 3 ];
		BZRestState( m, xyz );
		a[ 0 ] = xyz[ 0 ];
		a[ 1 ] = xyz[ 1 ];
		a[ 2 ] = xyz[ 2 ];
		a[ 3 ] = 1.0;
		break;
	}
	case Reaction::CDIMA:
		a[ 0 ] = p[ P_LE_A ] / 5.0;
		a[ 1 ] = 1.0 + a[ 0 ] * a[ 0 ];
		break;
	case Reaction::IodineClock:
		a[ 0 ] = p[ P_CK_H2O2_0 ];
		a[ 1 ] = p[ P_CK_I_0 ];
		a[ 2 ] = 0.0;
		a[ 3 ] = p[ P_CK_S_0 ];
		break;
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
		a[ 0 ] = p[ P_DY_O2SAT ];//freshly mixed: aerated
		a[ 1 ] = p[ P_DY_GL0 ];
		if( r == Reaction::Valentine )
			b[ 2 ] = p[ P_DY_CTOT ];//resazurin, not yet reduced
		else
			a[ 2 ] = p[ P_DY_CTOT ];//the oxidised dye
		break;
	case Reaction::Chameleon:
		a[ 0 ] = p[ P_CH_MN0 ];
		a[ 3 ] = p[ P_CH_GL0 ];
		break;
	default: break;
	}
}

void WellMixedRhs( Reaction r, const Params& p, double light, double aeration, const double* a, const double* b,
                   double* da, double* db )
{
	for( int i = 0; i < 4; ++i )
		da[ i ] = db[ i ] = 0.0;
	const double k0 = p[ P_FLOW_K0 ];
	switch( r )
	{
	case Reaction::BZ:
	{
		const double eps = p[ P_BZ_EPS ], epsP = p[ P_BZ_EPSP ], q = p[ P_BZ_Q ], T0 = p[ P_BZ_T0 ], f = p[ P_BZ_F ];
		const double x = a[ 0 ], y = a[ 1 ], z = a[ 2 ], fuel = std::clamp( a[ 3 ], 0.0, 1.0 );
		const double phi = p[ P_BZ_PHIMAX ] * light;
		//Batch: the spent fuel slows every rate (T0 / fuel) and stops it at 0;
		//a pacemaker site runs faster.
		const double rate = fuel / T0 * ( 1.0 + p[ P_BZ_PACE ] * b[ 0 ] );
		da[ 0 ]           = rate * ( q * y - x * y + x * ( 1.0 - x ) ) / eps;
		da[ 1 ]           = rate * ( -q * y - x * y + f * z + phi ) / epsP;
		da[ 2 ]           = rate * ( x - z );
		da[ 3 ]           = -p[ P_BZ_FUELRATE ] * z;
		break;
	}
	case Reaction::CDIMA:
	{
		const double u = a[ 0 ], v = a[ 1 ];
		const double w  = p[ P_LE_WMAX ] * light;
		const double rr = u * v / ( 1.0 + u * u );
		const double rate = 1.0 / p[ P_LE_TSCALE ];
		da[ 0 ]           = rate * ( p[ P_LE_A ] - u - 4.0 * rr - w ) / p[ P_LE_SIGMA ];
		da[ 1 ]           = rate * p[ P_LE_B ] * ( u - rr + w );
		break;
	}
	case Reaction::IodineClock:
	{
		//The slow production only: the thiosulfate titration is an exact
		//operator-split step (StepClockTitration), not a rate.
		const double H2O2 = std::max( a[ 0 ], 0.0 ), I = std::max( a[ 1 ], 0.0 );
		const double rate = p[ P_CK_KP ] * H2O2 * I;
		da[ 0 ]           = -rate + k0 * ( p[ P_CK_H2O2_0 ] - a[ 0 ] );
		da[ 1 ]           = -2.0 * rate + k0 * ( p[ P_CK_I_0 ] - a[ 1 ] );
		da[ 2 ]           = rate - k0 * a[ 2 ];
		da[ 3 ]           = -k0 * a[ 3 ];//Flow feeds no thiosulfate: that comes by dose
		break;
	}
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
	{
		const double O2 = std::max( a[ 0 ], 0.0 ), GL = std::max( a[ 1 ], 0.0 ), ox = std::max( a[ 2 ], 0.0 ),
		             sq = std::max( a[ 3 ], 0.0 ), leuco = std::max( b[ 0 ], 0.0 ), rz = std::max( b[ 2 ], 0.0 );
		const double kOx = p[ P_DY_KOX ], kRed = p[ P_DY_K2 ] * p[ P_DY_OH ] * GL;
		const bool twoStep = p[ P_DY_TWOSTEP ] > 0.5f;
		//Resazurin -> resorufin, once (the valentine): two electrons from the sugar.
		const double vRz = p[ P_DY_KRZ ] * p[ P_DY_OH ] * GL * rz;
		double vOxRed, vRedOx;//reduction of the oxidised form, oxidation of the reduced
		double vSqRed = 0.0, vSqOx = 0.0;
		if( twoStep )
		{
			vOxRed = kRed * ox;                      //ox -> semiquinone (one electron)
			vSqRed = kRed * p[ P_DY_KSQ ] * sq;      //semiquinone -> leuco
			vSqOx  = kOx * O2 * sq;                  //semiquinone -> ox
			vRedOx = kOx * O2 * leuco;               //leuco -> semiquinone
			da[ 2 ] = -vOxRed + vSqOx;
			da[ 3 ] = vOxRed - vSqRed + vRedOx - vSqOx;
			db[ 0 ] = vSqRed - vRedOx;
			//Each one-electron step by the sugar costs half a gluconic acid;
			//each one-electron re-oxidation a quarter O2.
			da[ 1 ] = -0.5 * ( vOxRed + vSqRed ) - vRz;
			db[ 1 ] = 0.5 * ( vOxRed + vSqRed ) + vRz;
			da[ 0 ] = -0.25 * ( vRedOx + vSqOx );
		}
		else
		{
			vOxRed  = kRed * ox;
			vRedOx  = kOx * O2 * leuco;
			da[ 2 ] = -vOxRed + vRedOx + vRz;
			db[ 0 ] = vOxRed - vRedOx;
			da[ 1 ] = -vOxRed - vRz;
			db[ 1 ] = vOxRed + vRz;
			da[ 0 ] = -0.5 * vRedOx;
		}
		db[ 2 ] = -vRz;
		//Oxygen through the surface, raised by a shake or the stirring.
		da[ 0 ] += p[ P_DY_KLA ] * aeration * ( p[ P_DY_O2SAT ] - O2 );
		//Flow: glucose held, gluconic acid washed out; the dye stays (it is the
		//catalyst, it is not fed).
		da[ 1 ] += k0 * ( p[ P_DY_GL0 ] - a[ 1 ] );
		db[ 1 ] += -k0 * b[ 1 ];
		break;
	}
	case Reaction::Chameleon:
	{
		const double P = std::max( a[ 0 ], 0.0 ), M = std::max( a[ 1 ], 0.0 ), GL = std::max( a[ 3 ], 0.0 );
		const double alpha = p[ P_CH_KA ] * p[ P_CH_OH ] * GL, beta = p[ P_CH_KB ] * p[ P_CH_OH ] * GL;
		const double v1 = alpha * P, v2 = beta * M;
		da[ 0 ] = -v1;
		da[ 1 ] = v1 - v2 - k0 * a[ 1 ];
		da[ 2 ] = v2 - k0 * a[ 2 ];
		//Mn(VII) -> Mn(VI) is one electron, Mn(VI) -> Mn(IV) two; glucose gives two.
		da[ 3 ] = -0.5 * v1 - v2 + k0 * ( p[ P_CH_GL0 ] - a[ 3 ] );
		break;
	}
	default: break;
	}
}

//---------------------------------------------------------------------------
namespace
{
/// Solve ( LU ) x = rhs in place, n <= 16, partial pivoting done at factor time.
struct LU
{
	int n = 0;
	double m[ 16 * 16 ];
	int piv[ 16 ];
	bool Factor( const double* A, int count )
	{
		n = count;
		for( int i = 0; i < n * n; ++i )
			m[ i ] = A[ i ];
		for( int c = 0; c < n; ++c )
		{
			int best   = c;
			double big = std::fabs( m[ c * n + c ] );
			for( int r = c + 1; r < n; ++r )
				if( std::fabs( m[ r * n + c ] ) > big )
				{
					big  = std::fabs( m[ r * n + c ] );
					best = r;
				}
			piv[ c ] = best;
			if( best != c )
				for( int k = 0; k < n; ++k )
					std::swap( m[ c * n + k ], m[ best * n + k ] );
			if( m[ c * n + c ] == 0.0 )
				return false;
			for( int r = c + 1; r < n; ++r )
			{
				const double f = m[ r * n + c ] / m[ c * n + c ];
				m[ r * n + c ] = f;
				for( int k = c + 1; k < n; ++k )
					m[ r * n + k ] -= f * m[ c * n + k ];
			}
		}
		return true;
	}
	void Solve( double* x ) const
	{
		for( int c = 0; c < n; ++c )
			if( piv[ c ] != c )
				std::swap( x[ c ], x[ piv[ c ] ] );
		for( int r = 1; r < n; ++r )
			for( int c = 0; c < r; ++c )
				x[ r ] -= m[ r * n + c ] * x[ c ];
		for( int r = n - 1; r >= 0; --r )
		{
			for( int c = r + 1; c < n; ++c )
				x[ r ] -= m[ r * n + c ] * x[ c ];
			x[ r ] /= m[ r * n + r ];
		}
	}
};
} // namespace

StiffResult IntegrateStiff( int n, const RhsFn& rhs, double* y, double duration, double rtol, double atol,
                            const std::function< void( double, const double* )>& observe, double maxStep )
{
	StiffResult result;
	if( n <= 0 || n > 16 || duration <= 0.0 )
		return result;
	const double gamma = 1.0 + 1.0 / std::sqrt( 2.0 );
	std::vector< double > f0( n ), f1( n ), k1( n ), k2( n ), yt( n ), J( n * n ), A( n * n ), ynew( n ), yfd( n ), ffd( n );
	double t = 0.0, h = std::min( duration, maxStep > 0.0 ? maxStep : duration ) * 1e-3;
	h        = std::max( h, 1e-12 );
	LU lu;
	int guard = 0;
	while( t < duration && guard++ < 50000000 )
	{
		if( t + h > duration )
			h = duration - t;
		rhs( y, f0.data() );
		//The Jacobian by forward differences.
		for( int j = 0; j < n; ++j )
		{
			for( int i = 0; i < n; ++i )
				yfd[ i ] = y[ i ];
			const double dy = 1e-7 * std::max( std::fabs( y[ j ] ), 1e-9 );
			yfd[ j ] += dy;
			rhs( yfd.data(), ffd.data() );
			for( int i = 0; i < n; ++i )
				J[ i * n + j ] = ( ffd[ i ] - f0[ i ] ) / dy;
		}
		bool accepted = false;
		while( !accepted )
		{
			for( int i = 0; i < n * n; ++i )
				A[ i ] = -gamma * h * J[ i ];
			for( int i = 0; i < n; ++i )
				A[ i * n + i ] += 1.0;
			if( !lu.Factor( A.data(), n ) )
			{
				h *= 0.5;
				++result.rejected;
				continue;
			}
			for( int i = 0; i < n; ++i )
				k1[ i ] = f0[ i ];
			lu.Solve( k1.data() );
			for( int i = 0; i < n; ++i )
				yt[ i ] = y[ i ] + h * k1[ i ];
			rhs( yt.data(), f1.data() );
			for( int i = 0; i < n; ++i )
				k2[ i ] = f1[ i ] - 2.0 * k1[ i ];
			lu.Solve( k2.data() );
			double worst = 0.0;
			for( int i = 0; i < n; ++i )
			{
				ynew[ i ]        = y[ i ] + 1.5 * h * k1[ i ] + 0.5 * h * k2[ i ];
				const double err = std::fabs( 0.5 * h * ( k1[ i ] + k2[ i ] ) );
				const double tol = atol + rtol * std::max( std::fabs( y[ i ] ), std::fabs( ynew[ i ] ) );
				worst            = std::max( worst, err / tol );
			}
			bool finite = true;
			for( int i = 0; i < n; ++i )
				finite = finite && std::isfinite( ynew[ i ] );
			if( finite && worst <= 1.0 )
			{
				accepted = true;
				for( int i = 0; i < n; ++i )
					y[ i ] = ynew[ i ];
				t += h;
				++result.steps;
				const double grow = worst > 1e-12 ? 0.9 / std::sqrt( worst ) : 5.0;
				h *= std::clamp( grow, 0.2, 5.0 );
				if( maxStep > 0.0 )
					h = std::min( h, maxStep );
				if( observe )
					observe( t, y );
			}
			else
			{
				++result.rejected;
				h *= finite ? std::clamp( 0.9 / std::sqrt( std::max( worst, 1e-12 ) ), 0.1, 0.5 ) : 0.25;
				if( h < 1e-15 )
					return result;//stuck
			}
		}
	}
	return result;
}

} // namespace honeydew::chem
