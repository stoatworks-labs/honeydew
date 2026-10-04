#pragma once

#include "Controls.h"

#include <array>
#include <functional>
#include <vector>

/**
    The chemistry: every mechanism, every constant, every closed form, in one
    place, in mol/L and seconds and millimetres.

    docs/CHEMISTRY.md is the table of where each number comes from; the
    comments here name the source beside the number, and every STAND-IN is
    marked `STAND-IN` so a grep finds them all.

    The GPU steps each reaction per texel in single precision from the SAME
    parameter array `Params` that the double-precision references here
    consume (`WellMixedRhs`), indexed by the `P_*` constants below, so the
    two cannot drift apart silently: a parameter the shader reads is a
    parameter the reference reads.
*/
namespace honeydew::chem
{
//---------------------------------------------------------------------------
// Unit helpers. The dish is in millimetres and chemical seconds.
//---------------------------------------------------------------------------
constexpr double kCm2PerS_to_Mm2PerS = 100.0;///< 1 cm^2/s = 100 mm^2/s

//---------------------------------------------------------------------------
// The recipes: the 1x molarities each Recipe slider multiplies, in M.
//---------------------------------------------------------------------------
struct Recipe
{
	double oxidant   = 0.0;///< bromate / iodate / H2O2 / ClO2 / O2 (the air a Shake delivers) / KMnO4 per dose
	double acidBase  = 0.0;///< [H+] in the first four; [OH-] in the dye family and the chameleon
	double reductant = 0.0;///< malonic acid / malonic acid / thiosulfate / malonic acid / glucose
	double indicator = 0.0;///< catalyst / starch / starch / starch / the dye / the dish's starting KMnO4
};
/// The cited 1x recipe of a reaction (CHEMISTRY.md, "Recipes").
Recipe BaseRecipe( Reaction r );
/// The recipe at the four sliders' multipliers (0 = none of that reagent).
Recipe ScaledRecipe( Reaction r, double mOxidant, double mAcidBase, double mReductant, double mIndicator );

//---------------------------------------------------------------------------
// Belousov-Zhabotinsky: the Oregonator with the "Lo" rate constants, scaled
// to the Tyson-Fife two-variable model.
//
// Steps (FKN numbering as in Field & Försterling 1986, Tyson 1985):
//   1  A + Y (+2H)   -> X + P        k1 [M^-3 s^-1], rate k1 A H^2 Y
//   2  X + Y (+H)    -> 2P           k2 [M^-2 s^-1], rate k2 H X Y
//   3  2X            -> A + P        k3 [M^-1 s^-1], rate k3 X^2
//   4  A + X (+H)    -> 2X + 2Z      k4 [M^-2 s^-1], rate k4 A H X
//   5  B + Z         -> (f/2) Y      k5 = kc B [s^-1]
// with A = [BrO3-], H = [H+], B = [MA], X = [HBrO2], Y = [Br-], Z = [oxidised
// catalyst]. k1..k4 are the Field-Försterling 1986 ("Lo") values as tabulated
// in Table 1 of arXiv:chao-dyn/9711004 (Ipsen, Kramer, Sørensen); kc is a
// STAND-IN (Tyson's recommendation of order 1 M^-1 s^-1, from memory), see
// CHEMISTRY.md.
//---------------------------------------------------------------------------
struct Oregonator
{
	double k1 = 2.0;   ///< M^-3 s^-1, Field & Försterling 1986 via chao-dyn/9711004 Table 1
	double k2 = 3.0e6; ///< M^-2 s^-1, same
	double k3 = 3.0e3; ///< M^-1 s^-1, same
	double k4 = 42.0;  ///< M^-2 s^-1, same
	double kc = 1.0;   ///< M^-1 s^-1, k5 = kc [MA]: STAND-IN (Tyson 1985, from memory)
	double f  = 1.4;   ///< stoichiometric factor: a MODEL PARAMETER, not derivable from the recipe; Jahnke & Winfree 1991's excitable range starts here (STAND-IN choice)
	double DX = 1.0e-5;///< cm^2/s, HBrO2, Hynne & Sørensen 1993 via chao-dyn/9711004 Table 1
	double DY = 1.6e-5;///< cm^2/s, Br-, same
	double DZ = 0.6e-5;///< cm^2/s, the catalyst, same
};
extern const Oregonator kOregonator;

/// The Tyson scaling of the Oregonator for a recipe: the dimensionless
/// parameters and the scales that take u, v and tau back to M and s.
struct BZModel
{
	double eps      = 0.0;///< k5 / (k4 A H)
	double epsPrime = 0.0;///< 2 k3 k5 / (k2 k4 A H^2): the bromide time scale the two-variable model drops
	double q        = 0.0;///< 2 k1 k3 / (k2 k4)
	double f        = 0.0;
	double T0       = 0.0;///< s per unit of tau: 1 / k5
	double X0       = 0.0;///< M per unit of u: k4 A H / (2 k3)
	double Z0       = 0.0;///< M per unit of v: (k4 A H)^2 / (k3 k5)
	double Ctot     = 0.0;///< M of catalyst in the dish (the Indicator slot)
	double Du       = 0.0;///< mm^2/s
	double Dv       = 0.0;///< mm^2/s
	/// 2 sqrt( D k4 A H ): the leading-edge speed of the autocatalytic front
	/// in the small-threshold limit, mm/s -- what Field & Noyes 1974's
	/// v = 0.04 cm/s M^-1 sqrt( [H+][BrO3-] ) measures (CHEMISTRY.md).
	double frontSpeedMmPerS = 0.0;
};
BZModel MakeBZ( const Recipe& recipe, double f = kOregonator.f );
/// Field & Noyes 1974's empirical law, mm/s, for the comparison the README reports.
double FieldNoyesSpeedMmPerS( double H, double A );

/// The two-variable (Tyson-Fife) right-hand side in dimensionless time:
///   eps du/dtau = u - u^2 - ( f v + phi )( u - q ) / ( u + q ),  dv/dtau = u - v.
/// phi is the photo-Oregonator's light-induced bromide production (Krug,
/// Pohlmann & Kuhnert 1990; Kádár, Amemiya & Showalter 1997), 0 in the dark.
void TysonFifeRhs( const BZModel& m, double phi, double u, double v, double& du, double& dv );
/// The three-variable Oregonator (x, y, z dimensionless, Tyson's scaling),
/// which is what the GPU steps per texel: eps dx = qy - xy + x(1-x),
/// eps' dy = -qy - xy + f z + phi, dz = x - z. The photo term phi (light-made
/// bromide) sits in the bromide equation, its natural place.
///
/// Why three variables and not the Tyson-Fife two: with the Lo constants q
/// (9.5e-5) is of the order of eps' (1.8e-4 at the 1x recipe), so the bromide
/// relaxes at the rate ( q + x ) / eps' ~ 1 at rest -- not fast -- and the
/// reduction misses the period by about a quarter (--oregonator states it).
void OregonatorRhs( const BZModel& m, double phi, const double* xyz, double* d );
/// The model's own oscillation peak of z at this recipe (0 if it does not
/// oscillate): the scale on which the picture shows the catalyst oxidised.
/// Integrated in double and cached by (eps, eps', q, f).
double BZPeakZ( const BZModel& m );
/// The rest state of the three-variable model (x, y, z).
void BZRestState( const BZModel& m, double xyz[ 3 ] );

/// The photo-Oregonator's phi at full light (STAND-IN scale, dimensionless;
/// the arXiv:1601.00848 study finds wave extinction above phi ~ 0.024 at
/// f = 1.16, q = 0.002). The harness measures this model's own threshold.
constexpr double kPhiMax = 0.06;

//---------------------------------------------------------------------------
// CDIMA: the Lengyel-Epstein model.
//
//   MA + I2 -> IMA + I- + H+          r1 = k1a [MA][I2] / ( k1b + [I2] )
//   ClO2 + I- -> ClO2- + 1/2 I2       r2 = k2 [ClO2][I-]
//   ClO2- + 4 I- + 4 H+ -> Cl- + 2 I2 r3 = k3a [ClO2-][I-][H+] + k3b [ClO2-][I2][I-] / ( alpha + [I-]^2 )
//   S + I2 + I- <-> SI3-              K = k4 / k-4
// Constants: the estimated CIMA/CDIMA set at 4 C tabulated in Table 3 of
// arXiv:2504.02530 (quoting its ref. 48, Lengyel, Rábai & Epstein), with the
// diffusion coefficients listed there. Dimensionless (Lengyel & Epstein,
// Science 1991): u = [I-]/sqrt(alpha), v = k3b [I2] [ClO2-] / ( alpha k2 [ClO2] ),
// a = k1a [MA] [I2] / ( sqrt(alpha) k2 [ClO2] ( k1b + [I2] ) ), b = k3b [I2] / ( sqrt(alpha) k2 [ClO2] ),
// sigma = 1 + K [S] [I2], t' = k2 [ClO2] t, x' = sqrt( k2 [ClO2] / D_I- ) x, d = D_ClO2- / D_I-.
//   du/dt' = ( a - u - 4uv/(1+u^2) - w + lap u ) / sigma,  dv/dt' = b ( u - uv/(1+u^2) + w ) + d lap v
// w is the photochemical term of Muñuzuri, Dolnik, Zhabotinsky & Epstein 1999
// (light consumes iodide and makes chlorite), 0 in the dark.
//---------------------------------------------------------------------------
struct LengyelEpstein
{
	double k1a   = 6.2e-4;///< s^-1
	double k1b   = 5.0e-5;///< M
	double k2    = 900.0; ///< M^-1 s^-1
	double k3a   = 100.0; ///< M^-2 s^-1
	double k3b   = 9.2e-5;///< s^-1
	double alpha = 1.0e-15;///< M^2, the cut-off the model needs at [I-] -> 0
	double K     = 1.0e8; ///< M^-1 (k4 / k-4 = 1e8 / 1), starch-triiodide
	double DI    = 0.7e-5;///< cm^2/s, I-
	double DClO2m = 0.75e-5;///< cm^2/s, ClO2-
	double I2    = 1.0e-3;///< M, the iodine the fed gel holds (the arXiv paper's estimate from the KI feed): fixed in the recipe
};
extern const LengyelEpstein kLE;

struct LEModel
{
	double a = 0, b = 0, sigma = 1, d = 1;
	double tScale = 0;///< s per unit of t'
	double xScale = 0;///< mm per unit of x'
	double Du = 0, Dv = 0;///< mm^2/s: D_I- / sigma (the 1/sigma multiplies the Laplacian too) and D_ClO2-
	double u0 = 0, v0 = 0;///< the steady state: u0 = a/5, v0 = 1 + u0^2
	bool turing = false;  ///< the steady state is Turing unstable
	double kc = 0;        ///< critical (fastest growing) wavenumber, per unit of x'
	double lambdaMm = 0;  ///< 2 pi / kc in mm
	double sigmaCritical = 0;///< the sigma (at this d) where the Turing instability sets in
};
LEModel MakeLE( const Recipe& recipe );
/// The Lengyel-Epstein right-hand side in t' (no diffusion).
void LERhs( const LEModel& m, double w, double u, double v, double& du, double& dv );
/// The light term at full light (STAND-IN scale; Muñuzuri et al. 1999 report
/// suppression of the pattern above a few mW/cm^2, which this maps to w ~ a/4).
constexpr double kWMaxFraction = 0.25;

//---------------------------------------------------------------------------
// The iodine clock (Harcourt-Esson).
//   H2O2 + 2 I- + 2 H+ -> I2 + 2 H2O     rate ( k1 + k2 [H+] ) [H2O2][I-]
//   I2 + 2 S2O3^2- -> 2 I- + S4O6^2-     k = 7.8e9 M^-1 s^-1 (Scheper & Margerum 1992): instantaneous here
//   I2 + I- <-> I3-                      K = 722 M^-1 (Ramette & Sandford 1965)
// k1, k2: Liebhafsky & Mohammad 1933 as commonly quoted (the paper's table
// was not reachable from this build): STAND-IN status until read in the paper.
//---------------------------------------------------------------------------
struct ClockKinetics
{
	double k1   = 0.0115;///< M^-1 s^-1, acid-independent path
	double k2   = 0.175; ///< M^-2 s^-1, acid-dependent path
	double KI3  = 722.0; ///< M^-1
	double kThio = 7.8e9;///< M^-1 s^-1 (treated as infinitely fast; the error is bounded in --clock)
	double iodide = 0.05;///< M, KI in the 1x recipe (fixed: not a slider)
	/// Starch binds triiodide into its amylose helix: a Langmuir site model,
	/// bound = sites K_s [I3-] / ( 1 + K_s [I3-] ), K_s STAND-IN 1e5 M^-1 (the
	/// complex is known to be very stable; no site constant was found to cite).
	double KStarch = 1.0e5;
	double DH2O2 = 1.4e-5;///< cm^2/s STAND-IN (order of a small molecule)
	double DI    = 2.0e-5; ///< cm^2/s, I- (CRC Handbook limiting ionic value, from memory: STAND-IN status)
	double DI2   = 1.36e-5;///< cm^2/s STAND-IN (Cantrel et al. 1997 report values near this)
	double DS2O3 = 1.1e-5; ///< cm^2/s STAND-IN
};
extern const ClockKinetics kClock;
/// ( k1 + k2 H ) in M^-1 s^-1.
double ClockRateConstant( double H );
/// The thiosulfate a well-mixed cell has consumed by t: the peroxide H(t)
/// makes iodine at k' H I0 (iodide I0 held: the thiosulfate regenerates it)
/// and each iodine takes two thiosulfates. In Batch H decays as H0 e^( -k' I0 t ):
///   S( t ) = 2 H0 ( 1 - e^( -k' I0 t ) ).
/// In Flow (feed k0 of peroxide at Hfeed, the thiosulfate washed at k0) H
/// relaxes from H0 to Hs = Hfeed k0 / ( k0 + k' I0 ) at lambda = k0 + k' I0, and
///   S( t ) = 2 k' I0 [ Hs ( e^( k0 t ) - 1 ) / k0 + ( H0 - Hs )( 1 - e^( -k' I0 t ) ) / ( k' I0 ) ]
/// (the dose needed for a switch at t; a dose S0 switches when S( t ) = S0).
double ClockDoseForSwitch( double tSwitch, double H0, double Hfeed, double I0, double H, double k0 = 0.0 );
/// The switch time of a dose S0 (the inverse of the above; a negative number
/// if the thiosulfate can never run out).
double ClockSwitchTime( double H0, double Hfeed, double I0, double H, double S0, double k0 = 0.0 );
/// The triiodide a starch holds, by the Langmuir site model.
double StarchBound( double I3free, double sites, double KStarch );
/// Iodine speciation: total iodine T = [I2] + [I3-] and free iodide F = [I-]
/// (the iodide not in I3-), K [I2][I-] = [I3-]. Solves the quadratic.
void IodineSpeciation( double totalIodine, double totalIodide, double K, double& I2, double& I3, double& iodideFree );

//---------------------------------------------------------------------------
// The blue-bottle family (Pons, Sagués, Bees & Sørensen, J. Phys. Chem. B 104,
// 2251 (2000), their kinetics and recipe):
//   V1 = k1 [O2][MBH]           k1 = 2000 M^-1 s^-1          (MBH + 1/2 O2 -> MB+)
//   V2 = kobs [MB+],  kobs = k2 [OH-][GL] = 0.0042 s^-1 at [OH-] 0.020, [GL] 0.054 M
// so k2 = 3.89 M^-2 s^-1 (derived from their kobs and recipe). O2 saturation
// 2.6e-4 M (air-saturated water). The same kinetics stand for indigo carmine
// (traffic light) and resazurin (valentine): STAND-IN by analogy, said so.
//---------------------------------------------------------------------------
struct DyeKinetics
{
	double kOx   = 2000.0;///< M^-1 s^-1, Pons 2000
	double k2    = 0.0042 / ( 0.020 * 0.054 );///< M^-2 s^-1, derived from Pons 2000's kobs
	double O2sat = 2.6e-4;///< M, air-saturated water (Pons 2000; Benson & Krause 1984 give 8.26 mg/L = 2.58e-4 M at 25 C)
	double DO2   = 2.11e-5;///< cm^2/s, Pons 2000/2008
	double Ddye  = 4.0e-6; ///< cm^2/s, MB+/MBH, Pons 2008 Table I
	double DGL   = 6.7e-6; ///< cm^2/s, glucose/gluconic acid, Pons 2008 Table I
	/// Oxygen enters a STILL layer by diffusion from its free surface; the 2-D
	/// engine has no depth, so its column takes oxygen at k_L a = 2 D / d^2
	/// (the mean diffusion distance to the middle of a layer of depth d): a
	/// CLOSURE, not a measurement. A 1.5 mm layer is then fed faster than it
	/// consumes at the 1x recipe and stays blue, as a thin dish does; a 7 mm
	/// layer fades (Pons 2000's blue top layer is 1-3 mm). A Shake multiplies
	/// it (kShakeAeration) for a moment; Stir by ( 1 + kStirAeration Stir^2 ).
	double kShakeAeration = 200.0;///< STAND-IN
	double kStirAeration  = 20.0; ///< STAND-IN
	/// Indigo carmine's two one-electron steps: the semiquinone forms at the
	/// dye's rate and is reduced on kSemiquinone times as fast (STAND-IN
	/// ratio; the measured step rates are in J. Chem. Educ. 101, 2505 (2024),
	/// not reachable from this build). Its re-oxidation uses kOx both steps.
	double kSemiquinone = 1.0;
	/// Indigo carmine's acid-base pKa: reported as 11.17 and 12.99 by two
	/// methods, with an indicator range of 11.4 (blue) to 13.0 (yellow).
	/// 12.6 is used, inside that span, so that the 1x recipe (pH 12.9) reads
	/// green as the demonstration does: a STAND-IN choice.
	double pKaIC = 12.6;
	/// Resazurin -> resorufin: irreversible, at the dye rate (STAND-IN by analogy).
	double kResazurin = 0.0042 / ( 0.020 * 0.054 );
	/// Gluconic acid's excess density over glucose: Pons 2008 Table I, 0.044
	/// g/(cm^3 M); Pons 2000 measured 1.0008 vs 1.0032 g/mL at 0.055 M, 25 C.
	double densityPerM = 0.044;
};
extern const DyeKinetics kDye;
/// How long a shaken cell stays oxidised: the oxygen it holds over the rate it
/// is consumed while the dye is fully oxidised, T = O2 / ( 1/2 k2 [OH-][GL] Ctot ).
double DyeBlueDuration( double O2, double OH, double GL, double dyeTotal );
/// The oxygen a shake must deliver for half the dye to be reduced again at
/// tFade, from a cell holding `ox0` of oxidised dye: the oxidising
/// equivalents E = 2 O2 + ox fall at kRed Ctot (the dye oxidised throughout)
/// less the surface's feed 2 kLa O2sat, until E = Ctot / 2:
///   O2 = ( tFade ( kRed Ctot - 2 kLa O2sat ) - ox0 + Ctot / 2 ) / 2.
double DyeOxygenForFade( double tFade, double kRed, double dyeTotal, double ox0, double kLa, double O2sat );

//---------------------------------------------------------------------------
// The chemical chameleon: permanganate reduced by glucose in alkali, a
// pseudo-first-order chain MnO4- -> MnO4^2- -> MnO2 (colloid), each step
// first order in the Mn species, in [OH-] and in glucose (Odebunmi & Owalude
// 2008 and others find first order in substrate, oxidant and OH-). The
// constants are STAND-INS calibrated to the demonstration's timings (purple
// to green in ~30 s, green to brown in ~2 min at 0.25 M NaOH, 0.05 M glucose).
//---------------------------------------------------------------------------
struct ChameleonKinetics
{
	double kA   = 4.0; ///< M^-2 s^-1 STAND-IN: MnO4- + e- (from glucose, OH-) -> MnO4^2-
	double kB   = 0.7; ///< M^-2 s^-1 STAND-IN: MnO4^2- -> MnO2
	double DMn  = 1.5e-5;///< cm^2/s STAND-IN for the two oxyanions
	double DMnO2 = 1.0e-7;///< cm^2/s: a colloid barely diffuses (STAND-IN)
	double DGL  = 6.7e-6; ///< cm^2/s glucose (Pons 2008)
};
extern const ChameleonKinetics kChameleon;
/// For a dose P of permanganate at t = 0: the manganate peak lands at
/// ln( beta / alpha ) / ( beta - alpha ), alpha = kA [OH-][GL], beta = kB [OH-][GL]
/// -- independent of the dose, which is why Clock Sync TIMES the chameleon's
/// dose rather than sizing it (AGENTS.md). In Flow both oxyanions are washed
/// out at k0, which adds k0 to each rate.
double ChameleonGreenPeakTime( double OH, double GL, double k0 = 0.0 );

//---------------------------------------------------------------------------
// Briggs-Rauscher: De Kepper & Epstein, JACS 104, 49 (1982), the ten-step
// mechanism with its rate constants AS TRANSCRIBED by Binous (Wolfram
// Demonstration) and the Paredes thesis (University of Florida), the only
// copy reachable from this build. See CHEMISTRY.md for the chain of custody
// and what the transcription did and did not reproduce.
//   A = I-, B = I2, C = IO3-, D = HOI, E = HIO2, F = IO2., G = Mn(OH)2+,
//   I = HO2., J = malonic acid, K = H2O2; H+ constant; Mn2+ = MnTotal - G.
//---------------------------------------------------------------------------
constexpr int kBRSpecies = 10;
enum BRIndex
{
	BR_I = 0, BR_I2, BR_IO3, BR_HOI, BR_HIO2, BR_IO2, BR_MN3, BR_HO2, BR_MA, BR_H2O2
};
struct BRKinetics
{
	double r1 = 1.43e3, r2 = 2.0e10, r3 = 3.1e12, rm3 = 2.2, r4 = 7.3e3, rm4 = 1.7e7, r5 = 6.0e5, r6 = 1.0e4, rm6 = 1.0e4,
	       r7 = 3.2e4, r8 = 7.5e5, r9 = 40.0, r10 = 37.0, c9 = 1.0e4;
	double mnTotal = 0.02; ///< M, the manganese of the 1x recipe (a demonstration's 0.02 M MnSO4)
	double k0CSTR  = 1.0 / 800.0;///< s^-1: a residence of 800 s sustains the oscillation at the 1x recipe (1/156 and 1/400 quench it in the model; CHEMISTRY.md)
	double DAll    = 1.5e-5;///< cm^2/s STAND-IN for every iodine species (used only between the CPU grid's cells)
};
extern const BRKinetics kBR;
/// dy/dt for the ten species, H+ held at `H`, with a CSTR feed `k0` towards
/// `feed` (nullptr for batch).
void BRRhs( const BRKinetics& k, double H, double k0, const double* feed, const double* y, double* dy );
/// The BR's fresh state for a recipe (iodate = oxidant, H+ = acid, MA =
/// reductant; H2O2 fixed from the Binous recipe scaled by the oxidant slider).
void BRInitialState( const Recipe& recipe, double* y, double& H, double& H2O2 );

//---------------------------------------------------------------------------
// The parameter array the GPU and the references share. Fixed slots.
//---------------------------------------------------------------------------
constexpr int kParamCount = 32;
using Params              = std::array< float, kParamCount >;
enum ParamSlot
{
	// common
	P_REACTION = 0,
	P_FLOW_K0,     ///< s^-1, the CSTR feed rate in Flow; 0 in Batch
	P_BATCH,       ///< 1 in Batch (pools deplete), 0 in Flow
	// BZ
	P_BZ_EPS = 4, P_BZ_Q, P_BZ_F, P_BZ_T0, P_BZ_DU, P_BZ_DV, P_BZ_PHIMAX, P_BZ_FUELRATE, P_BZ_PACE, P_BZ_CTOT, P_BZ_EPSP, P_BZ_DY, P_BZ_ZREF,
	// CDIMA
	P_LE_A = 4, P_LE_B, P_LE_SIGMA, P_LE_D, P_LE_TSCALE, P_LE_DU, P_LE_WMAX,
	// clock
	P_CK_KP = 4, P_CK_H2O2_0, P_CK_I_0, P_CK_S_0, P_CK_DH2O2, P_CK_DI, P_CK_DI2, P_CK_DS,
	// dye family
	P_DY_KOX = 4, P_DY_K2, P_DY_OH, P_DY_O2SAT, P_DY_KLA, P_DY_DO2, P_DY_DDYE, P_DY_DGL, P_DY_CTOT, P_DY_KSQ, P_DY_PKA,
	P_DY_KRZ, P_DY_GL0, P_DY_TWOSTEP,
	// chameleon
	P_CH_KA = 4, P_CH_KB, P_CH_OH, P_CH_DMN, P_CH_DMNO2, P_CH_DGL, P_CH_GL0, P_CH_MN0,
};
/// The parameters for a reaction at a recipe; `catalyst` matters to BZ only.
/// `f` is the Oregonator's stoichiometric factor (the Excitability control;
/// the other reactions ignore it).
Params MakeParams( Reaction r, const Recipe& recipe, Catalyst catalyst, Reactor reactor, double depthMm, double stir, double f = kOregonator.f );
/// The fresh (Reset) state of a well-mixed cell, 8 channels (A0..3, B0..3),
/// in the GPU's layout for that reaction. BR is not here (BRInitialState).
/// A freshly poured dish. BZ starts a touch above its rest state (HBrO2 at
/// kFreshKick times the rest value): the three-variable rest state is an
/// UNSTABLE fixed point in the oscillatory regime, and a dish seeded exactly
/// on it and stirred hard (the whole-vessel relaxation holds it there) never
/// started in 1500 s of 0.1.0 (AGENTS.md). `atRest` is the harness's negative
/// control: the exact fixed point, as 0.1.0 seeded it.
void FreshState( Reaction r, const Params& p, double* a, double* b, bool atRest = false );
constexpr double kFreshKick = 1.01;
/// The well-mixed right-hand side of a reaction in the GPU's state layout, in
/// chemical seconds, with `light` in 0..1 (the photosensitive ones) and an
/// aeration multiplier for the dye family. The GPU step is checked against
/// an integration of THIS.
void WellMixedRhs( Reaction r, const Params& p, double light, double aeration, const double* a4, const double* b4,
                   double* da4, double* db4 );
/// The same question answered by the model itself: the oxygen dose into the
/// well-mixed state ( a, b ) at which the oxidised dye crosses half its total
/// downwards at tFade, by bisection over the integrated model (the closed form
/// above assumes the dye fully oxidised while any oxygen is left, which a
/// small dose does not grant). Returns O2sat when even saturation fades too
/// soon (the caller says so).
double DyeOxygenForFadeModel( Reaction r, const Params& p, const double* a4, const double* b4, double tFade, double* achieved = nullptr );


//---------------------------------------------------------------------------
// A stiff integrator in double for the references: ROS2 (Verwer, Spee, Blom &
// Hundsdorfer 1999), L-stable, second order, with a numerical Jacobian and
// step-doubling error control. `rhs( y, dy )`.
//---------------------------------------------------------------------------
using RhsFn = std::function< void( const double* y, double* dy ) >;
struct StiffResult
{
	long long steps    = 0;
	long long rejected = 0;
};
/// Integrate y over [0, duration] in place; `atol`/`rtol` per component.
/// `observe( t, y )` is called after every accepted step when given.
StiffResult IntegrateStiff( int n, const RhsFn& rhs, double* y, double duration, double rtol = 1e-7, double atol = 1e-13,
                            const std::function< void( double, const double* ) >& observe = nullptr, double maxStep = 0.0 );

} // namespace honeydew::chem
