#include "Shaders.h"

namespace honeydew::shaders
{
const char* const kVersion = "#version 410 core\n";

//===========================================================================
// The library. No #version, no main. The reaction and parameter indices
// MIRROR Controls.h and Chemistry.h; `hdtest --names` parses these defines
// and fails if they drift from the enums.
//===========================================================================
const char* const kCommon = R"(
#define R_BZ 0
#define R_BR 1
#define R_CLOCK 2
#define R_CDIMA 3
#define R_TRAFFIC 4
#define R_BOTTLE 5
#define R_VALENTINE 6
#define R_CHAMELEON 7

#define P_FLOW_K0 1
#define P_BATCH 2
#define P_BZ_EPS 4
#define P_BZ_Q 5
#define P_BZ_F 6
#define P_BZ_T0 7
#define P_BZ_DU 8
#define P_BZ_DV 9
#define P_BZ_PHIMAX 10
#define P_BZ_FUELRATE 11
#define P_BZ_PACE 12
#define P_BZ_CTOT 13
#define P_BZ_EPSP 14
#define P_BZ_DY 15
#define P_BZ_ZREF 16
#define P_LE_A 4
#define P_LE_B 5
#define P_LE_SIGMA 6
#define P_LE_D 7
#define P_LE_TSCALE 8
#define P_LE_DU 9
#define P_LE_WMAX 10
#define P_CK_KP 4
#define P_CK_H2O2_0 5
#define P_CK_I_0 6
#define P_CK_S_0 7
#define P_DY_KOX 4
#define P_DY_K2 5
#define P_DY_OH 6
#define P_DY_O2SAT 7
#define P_DY_KLA 8
#define P_DY_CTOT 12
#define P_DY_KSQ 13
#define P_DY_PKA 14
#define P_DY_KRZ 15
#define P_DY_GL0 16
#define P_DY_TWOSTEP 17
#define P_CH_KA 4
#define P_CH_KB 5
#define P_CH_OH 6
#define P_CH_GL0 10
#define P_CH_MN0 11

//= mirrored in Hash.h, Pcg(). Integer only: the same on every GPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
uint cellHash( ivec2 c, uint salt )
{
	return pcg( uint( c.x ) ^ pcg( uint( c.y ) ^ pcg( salt ) ) );
}
//A hash as a float in [0,1): the top 24 bits, exact.
float hash01( ivec2 c, uint salt )
{
	return float( cellHash( c, salt ) >> 8u ) * ( 1.0 / 16777216.0 );
}
)";

//===========================================================================
// What every pass that touches the state shares: the two state textures,
// the grid, the parameters, the vessel.
//===========================================================================
const char* const kStateCommon = R"(
uniform sampler2D StateA;
uniform sampler2D StateB;
uniform ivec2 Grid;
uniform float Params[32];
uniform int Reaction;
uniform int Vessel;        //0 full frame, 1 petri dish
uniform vec2 DishCentre;   //cells
uniform float DishRadius;  //cells

float P( int i )
{
	return Params[ i ];
}

bool inDish( ivec2 c )
{
	if( Vessel == 0 )
		return true;
	vec2 d = vec2( c ) + 0.5 - DishCentre;
	return dot( d, d ) <= DishRadius * DishRadius;
}
)";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//===========================================================================
// Seed: fresh reagents, the Seed's heterogeneity, the clip's excitation.
//===========================================================================
const char* const kSeedFragment = R"(
uniform vec4 FreshA;
uniform vec4 FreshB;
uniform uint Salt;
uniform int SeedFromClip;
uniform sampler2D Clip;
uniform float NoiseAmp;    //relative noise on the fresh state
uniform float PaceDensity; //pacemaker sites per 24x24 block (BZ)
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	vec4 a  = FreshA;
	vec4 b  = FreshB;
	float h = hash01( c, Salt );
	float bright = 0.0;
	if( SeedFromClip == 1 )
	{
		vec4 t = texture( Clip, ( vec2( c ) + 0.5 ) / vec2( Grid ) );
		bright = dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) * t.a;
	}
	if( Reaction == R_BZ )
	{
		//Dust: a few pacemaker sites, radius 2.5 cells, one per 24 x 24 block
		//with probability PaceDensity, placed by the block's hash.
		ivec2 blk = c / 24;
		uint hb   = cellHash( blk, Salt ^ 0x9e3779b9u );
		if( float( hb >> 8u ) * ( 1.0 / 16777216.0 ) < PaceDensity )
		{
			uint hc = pcg( hb );
			vec2 site = vec2( blk * 24 ) + 4.0 + 16.0 * vec2( float( hc & 0xFFFFu ), float( hc >> 16u ) ) / 65535.0;
			vec2 d    = vec2( c ) + 0.5 - site;
			if( dot( d, d ) < 6.25 )
				b.x = 1.0;
		}
		a.x *= 1.0 + NoiseAmp * ( h - 0.5 );
		if( bright > 0.5 )
		{
			//Excited where the clip is bright.
			a.y = 0.0;
			a.x = 1.0;
		}
	}
	else if( Reaction == R_CDIMA )
	{
		float h2 = hash01( c, Salt ^ 0x5bd1e995u );
		a.x *= 1.0 + NoiseAmp * ( h - 0.5 );
		a.y *= 1.0 + NoiseAmp * ( h2 - 0.5 );
		if( bright > 0.5 )
			a.x *= 1.5;
	}
	else if( Reaction == R_TRAFFIC || Reaction == R_BOTTLE || Reaction == R_VALENTINE )
	{
		if( SeedFromClip == 1 )
			a.x = FreshA.x * bright;//aerated where the clip is bright
	}
	else if( Reaction == R_CHAMELEON )
	{
		if( SeedFromClip == 1 )
			a.x = FreshA.x * bright;
	}
	if( !inDish( c ) )
	{
		a = vec4( 0.0 );
		b = vec4( 0.0 );
	}
	OutA = a;
	OutB = b;
}
)";

//===========================================================================
// Step, part A: the library of per-reaction substeps. Each takes the
// diffused state and advances it by Dt chemical seconds.
//===========================================================================
const char* const kStepFragmentA = R"(
uniform float Dt;
uniform float Aeration;    //multiplier on the surface transfer (shake)
float Light = 0.0;         //0..1 at this cell: the clip's light x Light Coupling, set in main()
uniform float DropAmount;  //the reaction's unit of a drop
uniform int DiffusionOffForTest;
uniform int PhotoOffForTest;
uniform int FuelOffForTest;

//ROS2 (Verwer, Spee, Blom & Hundsdorfer 1999), the scheme the harness's
//double references use: k1 = A^-1 F( s ), k2 = A^-1 ( F( s + dt k1 ) - 2 k1 ),
//s' = s + 1.5 dt k1 + 0.5 dt k2, A = I - gamma dt J, gamma = 1 + 1/sqrt(2).
//L-stable and second order, so the stiff bromide is carried and the step's
//error shrinks as dt^2.
const float ROS_GAMMA = 1.7071067811865475;

vec3 bzF( vec3 s, float rate, float eps, float epsP, float q, float f, float phi )
{
	return rate * vec3( ( q * s.y - s.x * s.y + s.x * ( 1.0 - s.x ) ) / eps, ( -q * s.y - s.x * s.y + f * s.z + phi ) / epsP, s.x - s.z );
}

//The three-variable Oregonator (Chemistry.h): x = HBrO2, y = Br-, z = the
//oxidised catalyst, on Tyson's scales; B.x marks a pacemaker site.
void stepBZ( inout vec4 a, inout vec4 b )
{
	float eps = P( P_BZ_EPS ), epsP = P( P_BZ_EPSP ), q = P( P_BZ_Q ), f = P( P_BZ_F );
	float phi  = PhotoOffForTest == 1 ? 0.0 : P( P_BZ_PHIMAX ) * Light;
	float fuel = FuelOffForTest == 1 ? 1.0 : clamp( a.w, 0.0, 1.0 );
	float rate = fuel / P( P_BZ_T0 ) * ( 1.0 + P( P_BZ_PACE ) * b.x );
	vec3 s = a.xyz;
	//Column-major: J[col][row] = dF_row / d(col).
	mat3 J = mat3( rate * ( 1.0 - 2.0 * s.x - s.y ) / eps, rate * ( -s.y ) / epsP, rate,
	               rate * ( q - s.x ) / eps, rate * ( -q - s.x ) / epsP, 0.0,
	               0.0, rate * f / epsP, -rate );
	mat3 Ai = inverse( mat3( 1.0 ) - ROS_GAMMA * Dt * J );
	vec3 k1 = Ai * bzF( s, rate, eps, epsP, q, f, phi );
	vec3 k2 = Ai * ( bzF( s + Dt * k1, rate, eps, epsP, q, f, phi ) - 2.0 * k1 );
	vec3 n  = s + 1.5 * Dt * k1 + 0.5 * Dt * k2;
	a.xyz   = max( n, vec3( 0.0 ) );
	if( FuelOffForTest == 0 )
		a.w = max( a.w - Dt * P( P_BZ_FUELRATE ) * s.z, 0.0 );
}

vec2 leF( vec2 s, float rate, float A, float B, float sigma, float w )
{
	float r = s.x * s.y / ( 1.0 + s.x * s.x );
	return rate * vec2( ( A - s.x - 4.0 * r - w ) / sigma, B * ( s.x - r + w ) );
}

void stepCDIMA( inout vec4 a, inout vec4 b )
{
	float A = P( P_LE_A ), B = P( P_LE_B ), sigma = P( P_LE_SIGMA );
	float w    = PhotoOffForTest == 1 ? 0.0 : P( P_LE_WMAX ) * Light;
	float rate = 1.0 / P( P_LE_TSCALE );
	vec2 s = a.xy;
	float den = 1.0 + s.x * s.x;
	float drdu = s.y * ( 1.0 - s.x * s.x ) / ( den * den ), drdv = s.x / den;
	mat2 J     = mat2( rate * ( -1.0 - 4.0 * drdu ) / sigma, rate * B * ( 1.0 - drdu ), rate * ( -4.0 * drdv ) / sigma, rate * B * ( -drdv ) );
	mat2 Ai    = inverse( mat2( 1.0 ) - ROS_GAMMA * Dt * J );
	vec2 k1    = Ai * leF( s, rate, A, B, sigma, w );
	vec2 k2    = Ai * ( leF( s + Dt * k1, rate, A, B, sigma, w ) - 2.0 * k1 );
	a.xy       = max( s + 1.5 * Dt * k1 + 0.5 * Dt * k2, vec2( 0.0 ) );
}

void stepClock( inout vec4 a, inout vec4 b )
{
	float H2O2 = max( a.x, 0.0 ), I = max( a.y, 0.0 ), I2 = max( a.z, 0.0 ), S = max( a.w, 0.0 );
	float k0 = P( P_FLOW_K0 );
	//The slow production, limited by what is there.
	float made = min( P( P_CK_KP ) * H2O2 * I * Dt, min( H2O2, 0.5 * I ) );
	H2O2 -= made;
	I -= 2.0 * made;
	I2 += made;
	//The thiosulfate takes the iodine straight back (7.8e9 M^-1 s^-1): exact.
	float x = min( I2, 0.5 * S );
	I2 -= x;
	S -= 2.0 * x;
	I += 2.0 * x;
	//Flow feeds peroxide and iodide, washes everything; no thiosulfate (that is the dose).
	H2O2 += k0 * Dt * ( P( P_CK_H2O2_0 ) - H2O2 );
	I += k0 * Dt * ( P( P_CK_I_0 ) - I );
	I2 -= k0 * Dt * I2;
	S -= k0 * Dt * S;
	a = vec4( H2O2, I, I2, S );
}

void stepDye( inout vec4 a, inout vec4 b )
{
	float O2 = max( a.x, 0.0 ), GL = max( a.y, 0.0 ), ox = max( a.z, 0.0 ), sq = max( a.w, 0.0 );
	float leuco = max( b.x, 0.0 ), GLA = max( b.y, 0.0 ), rz = max( b.z, 0.0 );
	float kOx = P( P_DY_KOX ), kRed = P( P_DY_K2 ) * P( P_DY_OH ) * GL, k0 = P( P_FLOW_K0 );
	//Oxygen through the surface, exact over the substep.
	O2 += ( P( P_DY_O2SAT ) - O2 ) * ( 1.0 - exp( -P( P_DY_KLA ) * Aeration * Dt ) );
	//Resazurin -> resorufin, once.
	float dRz = min( rz, P( P_DY_KRZ ) * P( P_DY_OH ) * GL * rz * Dt );
	rz -= dRz;
	ox += dRz;
	float electrons = 2.0 * dRz;//from the sugar
	if( P( P_DY_TWOSTEP ) > 0.5 )
	{
		//ox -> sq -> leuco by the sugar; leuco -> sq -> ox by oxygen.
		float dOxRed = min( ox, kRed * ox * Dt );
		float dSqRed = min( sq, kRed * P( P_DY_KSQ ) * sq * Dt );
		float kO     = kOx * O2 * Dt;
		float dLeOx  = leuco * ( 1.0 - exp( -kO ) );
		float dSqOx  = sq * ( 1.0 - exp( -kO ) );
		//Oxygen: a quarter per one-electron step; never below zero.
		float need = 0.25 * ( dLeOx + dSqOx );
		if( need > O2 )
		{
			float s = O2 / max( need, 1e-30 );
			dLeOx *= s;
			dSqOx *= s;
			need = O2;
		}
		O2 -= need;
		ox += -dOxRed + dSqOx;
		sq += dOxRed - dSqRed - dSqOx + dLeOx;
		leuco += dSqRed - dLeOx;
		electrons += dOxRed + dSqRed;
	}
	else
	{
		float dOxRed = min( ox, kRed * ox * Dt );
		float dLeOx  = leuco * ( 1.0 - exp( -kOx * O2 * Dt ) );
		float need   = 0.5 * dLeOx;
		if( need > O2 )
		{
			dLeOx *= O2 / max( need, 1e-30 );
			need = O2;
		}
		O2 -= need;
		ox += -dOxRed + dLeOx;
		leuco += dOxRed - dLeOx;
		electrons += 2.0 * dOxRed;
	}
	//Each two electrons the sugar gives is one gluconic acid.
	float sugar = 0.5 * electrons;
	GL -= sugar;
	GLA += sugar;
	GL += k0 * Dt * ( P( P_DY_GL0 ) - GL );
	GLA -= k0 * Dt * GLA;
	a = vec4( max( O2, 0.0 ), max( GL, 0.0 ), max( ox, 0.0 ), max( sq, 0.0 ) );
	b = vec4( max( leuco, 0.0 ), max( GLA, 0.0 ), max( rz, 0.0 ), b.w );
}

void stepChameleon( inout vec4 a, inout vec4 b )
{
	float Pm = max( a.x, 0.0 ), M = max( a.y, 0.0 ), D = max( a.z, 0.0 ), GL = max( a.w, 0.0 );
	float alpha = P( P_CH_KA ) * P( P_CH_OH ) * GL, beta = P( P_CH_KB ) * P( P_CH_OH ) * GL;
	float k0 = P( P_FLOW_K0 );
	float ea = exp( -alpha * Dt ), eb = exp( -beta * Dt );
	float Pn = Pm * ea;
	float Mn;
	if( abs( beta - alpha ) > 1e-6 * max( alpha, 1e-30 ) )
		Mn = M * eb + Pm * alpha / ( beta - alpha ) * ( ea - eb );
	else
		Mn = ( M + Pm * alpha * Dt ) * ea;
	float madeM     = Pm - Pn;        //Mn(VII) -> Mn(VI), one electron each
	float consumedM = M + madeM - Mn; //Mn(VI) -> Mn(IV), two electrons each
	float Dn = D + consumedM;
	GL -= 0.5 * ( madeM + 2.0 * consumedM );
	Mn -= k0 * Dt * Mn;
	Dn -= k0 * Dt * Dn;
	GL += k0 * Dt * ( P( P_CH_GL0 ) - GL );
	a = vec4( Pn, max( Mn, 0.0 ), max( Dn, 0.0 ), max( GL, 0.0 ) );
}
)";

//===========================================================================
// Step, part B: diffusion with no-flux edges, the drops and the bar, then
// the reaction's substep.
//===========================================================================
const char* const kStepFragmentB = R"(
uniform vec4 DiffA;        //mm^2/s per channel of A (0: no diffusion)
uniform vec4 DiffB;
uniform float CellMm;
uniform float Eddy;        //mm^2/s added to every diffusing channel
uniform int DropCount;
uniform vec3 Drops[4];     //cx, cy (cells), radius (cells)
uniform int BarOn;
uniform vec4 Bar;          //x0 y0 x1 y1 in cells
uniform float BarHalfWidth;
uniform float DoseAll;     //a uniform dose over the whole dish (Clock Sync, a Shake's air)
uniform int DoseOn;
uniform float LightCoupling;
uniform sampler2D Clip;
uniform int HasClip;
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;

vec4 nbr( sampler2D s, ivec2 c, ivec2 d, vec4 centre )
{
	ivec2 n = c + d;
	if( n.x < 0 || n.y < 0 || n.x >= Grid.x || n.y >= Grid.y || !inDish( n ) )
		return centre;
	return texelFetch( s, n, 0 );
}

vec4 diffuse( sampler2D s, ivec2 c, vec4 centre, vec4 D )
{
	if( DiffusionOffForTest == 1 )
		return centre;
	vec4 lap = nbr( s, c, ivec2( 1, 0 ), centre ) + nbr( s, c, ivec2( -1, 0 ), centre ) + nbr( s, c, ivec2( 0, 1 ), centre )
	           + nbr( s, c, ivec2( 0, -1 ), centre ) - 4.0 * centre;
	vec4 Dd = D + vec4( greaterThan( D, vec4( 0.0 ) ) ) * Eddy;
	return centre + Dt * Dd * lap / ( CellMm * CellMm );
}

float segmentDistance( vec2 p, vec2 a, vec2 b )
{
	vec2 ab = b - a;
	float t = clamp( dot( p - a, ab ) / max( dot( ab, ab ), 1e-12 ), 0.0, 1.0 );
	return length( p - ( a + t * ab ) );
}

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	vec4 a0 = texelFetch( StateA, c, 0 );
	vec4 b0 = texelFetch( StateB, c, 0 );
	if( !inDish( c ) )
	{
		OutA = a0;
		OutB = b0;
		return;
	}
	vec4 a = diffuse( StateA, c, a0, DiffA );
	vec4 b = diffuse( StateB, c, b0, DiffB );
	vec2 p = vec2( c ) + 0.5;
	if( HasClip == 1 && LightCoupling > 0.0 )
	{
		vec4 t = texture( Clip, p / vec2( Grid ) );
		Light  = LightCoupling * dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) * t.a;
	}
	if( DoseOn == 1 )
	{
		if( Reaction == R_CLOCK )
			a.w += DoseAll;
		else if( Reaction == R_TRAFFIC || Reaction == R_BOTTLE || Reaction == R_VALENTINE )
			a.x = max( a.x, DoseAll );
		else if( Reaction == R_CHAMELEON )
			a.x += DoseAll;
		else if( Reaction == R_CDIMA )
			a.x += DoseAll;
	}
	for( int i = 0; i < DropCount; ++i )
	{
		vec2 d = p - Drops[ i ].xy;
		if( dot( d, d ) <= Drops[ i ].z * Drops[ i ].z )
		{
			if( Reaction == R_BZ )
			{
				//Silver takes the bromide: the site fires.
				a.y = 0.0;
				a.x = 1.0;
			}
			else if( Reaction == R_CDIMA )
				a.x += DropAmount;//an iodide perturbation
			else if( Reaction == R_CLOCK )
				a.w += DropAmount;//thiosulfate
			else if( Reaction == R_TRAFFIC || Reaction == R_BOTTLE || Reaction == R_VALENTINE )
				a.x = max( a.x, DropAmount );//air-saturated water
			else if( Reaction == R_CHAMELEON )
			{
				//A drop of permanganate in water: it brings no glucose, so its
				//edge, where the dish's glucose diffuses in, reacts first.
				a.x += DropAmount;
				a.w = 0.0;
			}
		}
	}
	if( BarOn == 1 && Reaction == R_BZ && segmentDistance( p, Bar.xy, Bar.zw ) <= BarHalfWidth )
	{
		//A pipette dragged through the dish: the wave is erased and the
		//medium left refractory (bromide-rich, catalyst oxidised), so the two
		//ends curl.
		a.x = P( P_BZ_Q );
		a.y = max( a.y, 2.0 * P( P_BZ_F ) );
		a.z = max( a.z, P( P_BZ_ZREF ) );
	}
	if( Reaction == R_BZ )
		stepBZ( a, b );
	else if( Reaction == R_CDIMA )
		stepCDIMA( a, b );
	else if( Reaction == R_CLOCK )
		stepClock( a, b );
	else if( Reaction == R_TRAFFIC || Reaction == R_BOTTLE || Reaction == R_VALENTINE )
		stepDye( a, b );
	else if( Reaction == R_CHAMELEON )
		stepChameleon( a, b );
	OutA = a;
	OutB = b;
}
)";

//===========================================================================
// Advect: the stir bar's vortex, an exact rotation about the dish's centre:
// solid-body inside the core, decaying as 1/r^2 outside (a Rankine vortex).
//===========================================================================
const char* const kAdvectFragment = R"(
uniform float Angle;      //rad: Omega x the frame's chemical time
uniform float CoreCells;
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	vec2 p  = vec2( c ) + 0.5;
	if( !inDish( c ) )
	{
		OutA = texelFetch( StateA, c, 0 );
		OutB = texelFetch( StateB, c, 0 );
		return;
	}
	vec2 d  = p - DishCentre;
	float r = length( d );
	float theta = Angle * min( 1.0, ( CoreCells * CoreCells ) / max( r * r, 1e-6 ) );
	float cs = cos( -theta ), sn = sin( -theta );
	vec2 src = DishCentre + vec2( cs * d.x - sn * d.y, sn * d.x + cs * d.y );
	//Stay inside the dish (the wall is a wall) and the grid.
	if( Vessel == 1 )
	{
		vec2 e  = src - DishCentre;
		float l = length( e );
		if( l > DishRadius - 0.5 )
			src = DishCentre + e * ( DishRadius - 0.5 ) / l;
	}
	src = clamp( src, vec2( 0.5 ), vec2( Grid ) - 0.5 );
	vec2 uvs = src / vec2( Grid );
	OutA = texture( StateA, uvs );
	OutB = texture( StateB, uvs );
}
)";

//===========================================================================
// Relax: towards the grid's mean, the whole-vessel closure of hard stirring.
//===========================================================================
const char* const kRelaxFragment = R"(
uniform sampler2D MeanA;
uniform sampler2D MeanB;
uniform float Alpha;
uniform vec4 MaskA;   //1 for a channel that relaxes, 0 for one that does not
uniform vec4 MaskB;
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	vec4 a  = texelFetch( StateA, c, 0 );
	vec4 b  = texelFetch( StateB, c, 0 );
	if( inDish( c ) )
	{
		vec4 ma = texelFetch( MeanA, ivec2( 0 ), 0 );
		vec4 mb = texelFetch( MeanB, ivec2( 0 ), 0 );
		a += Alpha * MaskA * ( ma - a );
		b += Alpha * MaskB * ( mb - b );
	}
	OutA = a;
	OutB = b;
}
)";

//===========================================================================
// The mean over the dish, in two reductions: column sums (and the count of
// cells in the dish in B's spare channel is not needed: the count is a
// uniform), then the total.
//===========================================================================
const char* const kColumnsFragment = R"(
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;
void main()
{
	int x = int( gl_FragCoord.x );
	vec4 sa = vec4( 0.0 ), sb = vec4( 0.0 );
	for( int y = 0; y < Grid.y; ++y )
	{
		ivec2 c = ivec2( x, y );
		if( inDish( c ) )
		{
			sa += texelFetch( StateA, c, 0 );
			sb += texelFetch( StateB, c, 0 );
		}
	}
	OutA = sa;
	OutB = sb;
}
)";

const char* const kTotalFragment = R"(
uniform sampler2D ColumnsA;
uniform sampler2D ColumnsB;
uniform float CellCount;  //cells in the dish
layout( location = 0 ) out vec4 OutA;
layout( location = 1 ) out vec4 OutB;
void main()
{
	vec4 sa = vec4( 0.0 ), sb = vec4( 0.0 );
	for( int x = 0; x < Grid.x; ++x )
	{
		sa += texelFetch( ColumnsA, ivec2( x, 0 ), 0 );
		sb += texelFetch( ColumnsB, ivec2( x, 0 ), 0 );
	}
	OutA = sa / max( CellCount, 1.0 );
	OutB = sb / max( CellCount, 1.0 );
}
)";

//===========================================================================
// Colour: Beer-Lambert through the layer at the grid's resolution.
//
// Each cell's state becomes up to six (species, concentration) pairs; the
// absorbance at each of the 41 samples is sum eps_i( lambda_k ) c_i x depth;
// T_k = 10^-A. The source's colour is sum_k LightWeights[k] T_k (linear sRGB,
// the lightbox normalised to Y = 1). The Over's filter is the 3x3
// M = I + sum_k PrimaryWeights[k] ( T_k - 1 ): EXACTLY the identity for an
// empty dish, because every ( T_k - 1 ) is exactly 0 there.
//===========================================================================
const char* const kColourFragment = R"(
#define S_FERROIN 0
#define S_FERRIIN 1
#define S_RU2 2
#define S_RU3 3
#define S_CE4 4
#define S_I2 5
#define S_I3 6
#define S_STARCH_I3 7
#define S_CLO2 8
#define S_IC_BLUE 9
#define S_IC_YELLOW 10
#define S_IC_SEMI 11
#define S_IC_LEUCO 12
#define S_MB 13
#define S_MB_DIMER 14
#define S_RESAZURIN 15
#define S_RESORUFIN 16
#define S_MNO4 17
#define S_MNO4_2 18
#define S_MNO2 19
#define LAMBDAS 41

uniform sampler2D Eps;          //S_COUNT rows x 41 columns, M^-1 cm^-1
uniform vec3 LightWeights[LAMBDAS];
uniform mat3 PrimaryWeights[LAMBDAS];
uniform int IsEffect;
uniform float DepthCm;
uniform int Catalyst;           //BZ: 0 ferroin, 1 Ru(bpy)3, 2 cerium
uniform float KI3;              //triiodide formation constant
uniform float StarchSites;      //M of sites that bind one I3- each
uniform float KDimer;           //methylene blue dimerisation
uniform float ClO2Pool;         //M, CDIMA
uniform float LEu0;             //CDIMA steady-state u
uniform int NaiveDepthForTest;  //--beer's negative control: square the colour instead of doubling the depth
layout( location = 0 ) out vec4 Out0;
layout( location = 1 ) out vec4 Out1;
layout( location = 2 ) out vec4 Out2;

int sIdx[6];
float sConc[6];
int sCount = 0;
void add( int s, float c )
{
	if( sCount < 6 && c > 0.0 )
	{
		sIdx[ sCount ]  = s;
		sConc[ sCount ] = c;
		++sCount;
	}
}

//I2 + I- <-> I3-: the smaller root of K I3^2 - ( 1 + K( T + F ) ) I3 + K T F = 0.
void speciation( float T, float F, out float I2, out float I3 )
{
	if( T <= 0.0 || F <= 0.0 )
	{
		I2 = max( T, 0.0 );
		I3 = 0.0;
		return;
	}
	float bq   = 1.0 + KI3 * ( T + F );
	float disc = sqrt( max( bq * bq - 4.0 * KI3 * KI3 * T * F, 0.0 ) );
	I3         = clamp( 2.0 * KI3 * T * F / ( bq + disc ), 0.0, min( T, F ) );
	I2         = T - I3;
}

void iodineColours( float T, float F )
{
	float I2, I3;
	speciation( T, F, I2, I3 );
	float bound = min( I3, StarchSites );
	add( S_I2, I2 );
	add( S_I3, I3 - bound );
	add( S_STARCH_I3, bound );
}

void species( vec4 a, vec4 b )
{
	if( Reaction == R_BZ )
	{
		//The Oregonator's catalyst is unbounded; z over the model's own peak at
		//this recipe is the oxidised fraction of the dish's catalyst (AGENTS.md).
		float ctot = P( P_BZ_CTOT );
		float ox   = clamp( a.z / P( P_BZ_ZREF ), 0.0, 1.0 ) * ctot;
		if( Catalyst == 0 )
		{
			add( S_FERROIN, ctot - ox );
			add( S_FERRIIN, ox );
		}
		else if( Catalyst == 1 )
		{
			add( S_RU2, ctot - ox );
			add( S_RU3, ox );
		}
		else
			add( S_CE4, ox );
	}
	else if( Reaction == R_BR )
		iodineColours( a.x, a.y );
	else if( Reaction == R_CLOCK )
		iodineColours( a.z, a.y );
	else if( Reaction == R_CDIMA )
	{
		//The model's u is iodide on the alpha cut-off's scale, which does not
		//predict the absolute concentration; the starch-triiodide colour is
		//the model's u against its steady state, on the dish's sites (AGENTS.md).
		float frac = a.x / max( a.x + LEu0, 1e-30 );
		add( S_STARCH_I3, StarchSites * frac );
		add( S_CLO2, ClO2Pool );
	}
	else if( Reaction == R_TRAFFIC )
	{
		//The oxidised dye is an acid-base pair: blue below pKa, yellow above.
		float pH     = 14.0 + log( max( P( P_DY_OH ), 1e-14 ) ) / log( 10.0 );
		float yellow = 1.0 / ( 1.0 + pow( 10.0, P( P_DY_PKA ) - pH ) );
		add( S_IC_BLUE, a.z * ( 1.0 - yellow ) );
		add( S_IC_YELLOW, a.z * yellow );
		add( S_IC_SEMI, a.w );
		add( S_IC_LEUCO, b.x );
	}
	else if( Reaction == R_BOTTLE )
	{
		//Monomer m and dimer d: m + 2d = ox, d = K m^2.
		float ox = max( a.z, 0.0 );
		float m  = KDimer > 0.0 ? ( -1.0 + sqrt( 1.0 + 8.0 * KDimer * ox ) ) / ( 4.0 * KDimer ) : ox;
		add( S_MB, m );
		add( S_MB_DIMER, 0.5 * ( ox - m ) );
	}
	else if( Reaction == R_VALENTINE )
	{
		add( S_RESAZURIN, b.z );
		add( S_RESORUFIN, a.z );
	}
	else if( Reaction == R_CHAMELEON )
	{
		add( S_MNO4, a.x );
		add( S_MNO4_2, a.y );
		add( S_MNO2, a.z );
	}
}

void main()
{
	ivec2 c = ivec2( gl_FragCoord.xy );
	vec4 a  = texelFetch( StateA, c, 0 );
	vec4 b  = texelFetch( StateB, c, 0 );
	bool inside = inDish( c );
	if( inside )
		species( a, b );
	float depth = NaiveDepthForTest == 1 ? 0.5 * DepthCm : DepthCm;
	vec3 rgb = vec3( 0.0 );
	mat3 M   = mat3( 1.0 );
	for( int k = 0; k < LAMBDAS; ++k )
	{
		float A = 0.0;
		for( int i = 0; i < sCount; ++i )
			A += texelFetch( Eps, ivec2( k, sIdx[ i ] ), 0 ).r * sConc[ i ];
		float T = exp( -2.302585092994046 * A * depth );
		if( NaiveDepthForTest == 1 )
			T = T * T;//the naive per-sample square: equals the right answer here, so the
			          //negative control squares the COLOUR instead, below
		rgb += LightWeights[ k ] * T;
		M += PrimaryWeights[ k ] * ( T - 1.0 );
	}
	if( NaiveDepthForTest == 1 )
	{
		//The wrong model: the colour of half the depth, squared per channel.
		vec3 half1 = vec3( 0.0 );
		for( int k = 0; k < LAMBDAS; ++k )
		{
			float A = 0.0;
			for( int i = 0; i < sCount; ++i )
				A += texelFetch( Eps, ivec2( k, sIdx[ i ] ), 0 ).r * sConc[ i ];
			half1 += LightWeights[ k ] * exp( -2.302585092994046 * A * 0.5 * DepthCm );
		}
		rgb = half1 * half1;
	}
	float mask = inside ? 1.0 : 0.0;
	if( IsEffect == 1 )
	{
		Out0 = vec4( M[ 0 ], mask );
		Out1 = vec4( M[ 1 ], 0.0 );
		Out2 = vec4( M[ 2 ], 0.0 );
	}
	else
	{
		Out0 = vec4( rgb, mask );
		Out1 = vec4( 0.0 );
		Out2 = vec4( 0.0 );
	}
}
)";

//===========================================================================
// Composite: the picture. The colour textures are read with a hand-made
// bilinear filter from four texelFetches, written as a + t ( b - a ) so that
// four equal taps return their value EXACTLY (an empty dish hands the clip
// back bit for bit, Mix aside).
//===========================================================================
const char* const kCompositeFragment = R"(
uniform sampler2D Col0;
uniform sampler2D Col1;
uniform sampler2D Col2;
uniform sampler2D InputTexture;
uniform ivec2 Grid;
uniform int IsEffect;
uniform vec2 Raster;
uniform vec2 ViewOrigin;
uniform float MixAmount;
uniform float Gain;
uniform int Vessel;
uniform vec2 DishCentrePx;
uniform float DishRadiusPx;
out vec4 FragColor;

vec4 lerp4( vec4 a, vec4 b, float t )
{
	return a + t * ( b - a );
}

vec4 bilinear( sampler2D s, vec2 g )
{
	//g: position in cells. Texel i covers [i, i+1); its centre is i + 0.5.
	vec2 q  = g - 0.5;
	vec2 f  = floor( q );
	vec2 t  = q - f;
	ivec2 i = ivec2( f );
	ivec2 lo = ivec2( 0 ), hi = Grid - 1;
	ivec2 i00 = clamp( i, lo, hi ), i10 = clamp( i + ivec2( 1, 0 ), lo, hi );
	ivec2 i01 = clamp( i + ivec2( 0, 1 ), lo, hi ), i11 = clamp( i + ivec2( 1, 1 ), lo, hi );
	vec4 a = lerp4( texelFetch( s, i00, 0 ), texelFetch( s, i10, 0 ), t.x );
	vec4 b = lerp4( texelFetch( s, i01, 0 ), texelFetch( s, i11, 0 ), t.x );
	return lerp4( a, b, t.y );
}

void main()
{
	vec2 p = gl_FragCoord.xy - ViewOrigin;
	vec2 g = p / Raster * vec2( Grid );
	vec4 c0 = bilinear( Col0, g );
	if( IsEffect == 0 )
	{
		vec3 rgb = c0.rgb * Gain;
		if( Vessel == 1 )
		{
			//The dish's wall: a thin dark ring at the rim.
			float d = abs( length( p - DishCentrePx ) - DishRadiusPx );
			rgb *= 1.0 - 0.45 * clamp( 1.5 - d, 0.0, 1.0 );
		}
		FragColor = vec4( rgb, 1.0 );
		return;
	}
	vec4 clip = texelFetch( InputTexture, ivec2( p ), 0 );
	vec4 c1   = bilinear( Col1, g );
	vec4 c2   = bilinear( Col2, g );
	mat3 M    = mat3( c0.rgb, c1.rgb, c2.rgb );
	vec3 filtered = ( M * clip.rgb ) * Gain;
	FragColor = vec4( clip.rgb + MixAmount * ( filtered - clip.rgb ), clip.a );
}
)";

//===========================================================================
// Thumb: a small luminance copy of the clip, for Drop Position Brightest.
//===========================================================================
const char* const kThumbFragment = R"(
uniform sampler2D Clip;
uniform vec2 ThumbSize;
out vec4 FragColor;
void main()
{
	vec2 uv = gl_FragCoord.xy / ThumbSize;
	vec4 t  = texture( Clip, uv );
	FragColor = vec4( dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) * t.a, 0.0, 0.0, 1.0 );
}
)";

std::string Assemble( const char* a, const char* b, const char* c )
{
	std::string s = kVersion;
	s += kCommon;
	for( const char* piece : { a, b, c } )
		if( piece )
			s += piece;
	return s;
}

} // namespace honeydew::shaders
