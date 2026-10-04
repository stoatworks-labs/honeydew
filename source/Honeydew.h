#pragma once

#include "Audio.h"
#include "BrEngine.h"
#include "Chemistry.h"
#include "Clock.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Transport.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace honeydew
{
/**
    The plugin: the source (the dish on a lightbox) and, with `isEffect`, the
    Over effect (the clip is the lightbox). One class, two registrations; see
    SourcePlugin.cpp and EffectPlugin.cpp.

    Host indices are NOT ParamIds: each plugin declares its own dense list,
    `HostOrder( isEffect )`. The host-facing calls translate; everything
    inside works in ParamIds.
*/
class HoneydewPlugin : public CFFGLPlugin
{
public:
	explicit HoneydewPlugin( bool isEffect );
	~HoneydewPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// The base class's stub fails, and a failed default deletes the instance
	/// -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	bool IsEffect() const
	{
		return isEffect;
	}
	unsigned int ParamCount() const
	{
		return static_cast< unsigned int >( hostOrder.size() );
	}
	int HostIndexOf( unsigned int id ) const
	{
		return id < PT_COUNT ? idToHost[ id ] : -1;
	}
	void SetById( unsigned int id, float value );
	float GetById( unsigned int id ) const
	{
		return id < PT_COUNT ? params[ id ] : 0.0f;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	double ClockScale() const
	{
		return clockScale;
	}
	/// A grid of exactly this size, whatever Detail and the raster say.
	void SetGridForTest( int cols, int rows )
	{
		gridOverride = Grid { cols, rows };
	}
	/// On the next frame, advance the chemistry by exactly this many chemical
	/// seconds INSTEAD of the host clock's (substep-capped like any frame).
	void SetChemTimeForTest( double chemSeconds )
	{
		testChemSeconds = chemSeconds;
		testChemOn      = true;
	}
	/// Uncapped: as many substeps as the stability bound asks for.
	void SetUncappedForTest( bool on )
	{
		uncapped = on;
	}
	/// Multiply the stable substep by this (0.5: twice as many substeps), to
	/// measure the step's own discretisation error.
	void SetSubstepScaleForTest( double scale )
	{
		substepScale = scale;
	}
	BrEngine& BRForTest()
	{
		return br;
	}
	/// On the next frame, before any step: the state is exactly these cells
	/// (cols x rows x 4 floats for A and for B, row 0 at the bottom).
	void LoadStateForTest( const std::vector< float >& a, const std::vector< float >& b )
	{
		pendingA    = a;
		pendingB    = b;
		loadPending = true;
	}
	/// A drop at a cell position on the next frame.
	void DropForTest( double cx, double cy, double radiusCells )
	{
		testDrops.push_back( { cx, cy, radiusCells } );
	}
	/// A uniform dose of the reaction's dosed reagent over the whole dish on the next frame.
	void DoseForTest( double amount )
	{
		testDose   = amount;
		testDoseOn = true;
	}
	/// A Break Wave bar between two cell positions on the next frame.
	void BarForTest( double x0, double y0, double x1, double y1, double halfWidthCells )
	{
		testBar     = { x0, y0, x1, y1, halfWidthCells };
		testBarOn   = true;
	}
	/// Override one Params slot (a wrong model for a negative control).
	void SetParamOverrideForTest( int slot, float value )
	{
		overrides.push_back( { slot, value } );
	}
	void ClearParamOverridesForTest()
	{
		overrides.clear();
	}
	void SetDiffusionOffForTest( bool on )
	{
		diffusionOff = on;
	}
	void SetPhotoOffForTest( bool on )
	{
		photoOff = on;
	}
	void SetFuelOffForTest( bool on )
	{
		fuelOff = on;
	}
	void SetNaiveDepthForTest( bool on )
	{
		naiveDepth = on;
	}
	void SetUnprimedForTest( bool on )
	{
		unprimed = on;
	}
	void SetFloatClockForTest( bool on )
	{
		floatClock = on;
	}
	void SetClearOnResizeForTest( bool on )
	{
		clearOnResize = on;
	}
	/// Clock Sync's dose placed on the wrong closed form (the acid term dropped).
	void SetWrongDoseForTest( bool on )
	{
		wrongDose = on;
	}
	/// --stir's negative control: Stir ignored (no vortex, no eddies, no relaxation).
	void SetStirOffForTest( bool on )
	{
		stirOff = on;
	}
	/// --units' negative control: the cell is 0.1 mm whatever Dish Width says.
	void SetFixedCellForTest( bool on )
	{
		fixedCell = on;
	}
	/// The transport the harness drives.
	Transport& TransportForTest()
	{
		return transport;
	}
	/// Mean of the dish's A and B, as read back after the last frame.
	void MeanState( double a[ 4 ], double b[ 4 ] ) const
	{
		for( int i = 0; i < 4; ++i )
		{
			a[ i ] = meanA[ i ];
			b[ i ] = meanB[ i ];
		}
	}
	GLuint StateATextureID() const
	{
		return stateA[ cur ].TextureID();
	}
	GLuint StateBTextureID() const
	{
		return stateB[ cur ].TextureID();
	}
	Grid CurrentGrid() const
	{
		return grid;
	}
	double CellMm() const
	{
		return cellMm;
	}
	/// Chemical seconds the chemistry has actually advanced since the last
	/// Reset, and since the plugin loaded.
	double ChemicalTime() const
	{
		return chemTime;
	}
	double ChemicalTimeTotal() const
	{
		return chemTimeTotal;
	}
	/// Chemical seconds the substep cap dropped, and frames it bit in.
	double LostChemicalTime() const
	{
		return lostChem;
	}
	long long CappedFrames() const
	{
		return cappedFrames;
	}
	long long SubstepsTaken() const
	{
		return substepsTaken;
	}
	double LastHostDt() const
	{
		return lastHostDt;
	}
	unsigned long long Onsets() const
	{
		return onsetsUsed;
	}
	unsigned long long DropsMade() const
	{
		return dropsMade;
	}
	unsigned long long DosesMade() const
	{
		return dosesMade;
	}
	/// The last dose Clock Sync placed (its amount, and the real seconds it aimed at).
	double LastDoseAmount() const
	{
		return lastDoseAmount;
	}
	double LastDoseAimSeconds() const
	{
		return lastDoseAim;
	}
	/// Host seconds (the plugin's own clock) at which the last dose was placed.
	double LastDoseHostTime() const
	{
		return lastDoseHostTime;
	}
	double HostNow() const
	{
		return now;
	}
	const BrEngine& BR() const
	{
		return br;
	}
	chem::Params CurrentParams() const;
	chem::Recipe CurrentRecipe() const;
	Reaction CurrentReaction() const;
	bool LastFrameSeeded() const
	{
		return seededThisFrame;
	}

private:
	struct DropRequest
	{
		double cx, cy, radius;
	};
	struct BarRequest
	{
		double x0, y0, x1, y1, halfWidth;
	};

	void UpdateClock();
	bool ensureBuffers( const Grid& want );
	void seed();
	void stepGPU( int substeps, double dt, const std::vector< DropRequest >& drops, double dropAmount, bool bar,
	              const BarRequest& barReq, bool dose, double doseAmount, double aeration, double eddy );
	void advect( double angle );
	void reduceMean();
	void relax( double alpha );
	void colour( const FFGLTextureStruct* input );
	void composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width, int height );
	void readMean();
	bool brightestCell( const FFGLTextureStruct* input, double& cx, double& cy );
	void uploadBR();
	uint32_t seedSalt() const;
	void swapState()
	{
		cur = 1 - cur;
	}

	const bool isEffect;
	const std::vector< unsigned int >& hostOrder;
	int idToHost[ PT_COUNT ];
	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader seedShader, stepShader, advectShader, relaxShader, columnsShader, totalShader, colourShader,
		compositeShader, thumbShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer stateA[ 2 ], stateB[ 2 ];
	PassBuffer colour0, colour1, colour2;
	PassBuffer columnsA, columnsB, totalA, totalB;
	PassBuffer thumb;
	GLuint epsTexture = 0, blankClip = 0;
	GLuint mrtFBO     = 0;
	int cur           = 0;
	Grid grid, gridOverride;
	double cellMm = 0.0;
	std::vector< float > lightWeights, primaryWeights;
	int lightWeightsFor = -1;

	//Time (the clock unit vote is conway's, from rosette via radar).
	double hostTime = -1.0, lastRawTime = -1.0, lastWallTime = -1.0, wallStart = -1.0;
	double clockScale = 0.0;
	int secondsVotes = 0, millisVotes = 0;
	double now = 0.0;
	bool settledJump = false;
	ChemicalClock clock;
	Transport transport;
	double lastHostDt   = 0.0;
	double chemTime     = 0.0;
	double chemTimeTotal = 0.0;
	double lostChem     = 0.0;
	long long cappedFrames = 0, substepsTaken = 0;
	long long framesSinceCapLog = 0;

	//The dish.
	bool needSeed       = true;
	bool seededThisFrame = false;
	uint32_t seedSerial = 0;
	int lastSeed = -1, lastReaction = -1, lastDetail = -1;
	bool resetHeld = false, dropHeld = false, barHeld = false, shakeHeld = false;
	std::vector< DropRequest > pendingDrops, testDrops;
	BarRequest testBar {};
	bool testBarOn = false, barPending = false;
	bool shakePending = false;
	double shakeAge   = 1e9;//real seconds since the last shake
	double autoDropAccumulator = 0.0;
	unsigned long long dropsMade = 0, dosesMade = 0, onsetsUsed = 0;
	double lastDoseAmount = 0.0, lastDoseAim = 0.0, lastDoseHostTime = -1.0;
	//Clock Sync's state machine.
	bool syncArmed      = true;
	double chameleonDoseAt = -1.0;//host seconds
	double meanA[ 4 ] = {}, meanB[ 4 ] = {};
	bool meanValid = false;
	int lastWidth = 0, lastHeight = 0;
	//The vessel this frame, in cells.
	int vesselNow          = 0;
	double dishCentre[ 2 ] = { 0.0, 0.0 };
	double dishRadiusCells = 0.0;
	double dishCells       = 0.0;
	GLuint lastClipTexture = 0;

	BrEngine br;
	bool brActive = false;

	audio::Analyser analyser;
	bool unprimed = false;

	//Harness hooks.
	bool testChemOn        = false;
	bool testDoseOn        = false;
	double testDose        = 0.0;
	double syncDoseAt      = -1.0;//host seconds: the held dose's time
	double testChemSeconds = 0.0;
	bool uncapped          = false;
	double substepScale    = 1.0;
	std::vector< float > pendingA, pendingB;
	bool loadPending = false;
	std::vector< std::pair< int, float > > overrides;
	bool diffusionOff = false, photoOff = false, fuelOff = false, naiveDepth = false, floatClock = false,
	     clearOnResize = false, wrongDose = false, stirOff = false, fixedCell = false;
	double lastNow = -1.0;
};

} // namespace honeydew
