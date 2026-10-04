#pragma once

#include "Chemistry.h"

#include <cstdint>
#include <vector>

/**
    The Briggs-Rauscher engine: the De Kepper-Epstein mechanism in DOUBLE on a
    coarse CPU grid, because its ten species span ten decades of concentration
    and the stiff steps (3e12 M^-2 s^-1 against 2.2 s^-1) are not a job for
    per-texel float (AGENTS.md).

    Every cell is a well-mixed BR solution; the cells exchange matter by a
    five-point diffusion with one STAND-IN coefficient for every iodine species,
    plus the eddy diffusivity Stir adds, and relax towards the grid's mean at
    Stir's whole-vessel rate (the same closure the GPU engines use). No stir-bar
    advection: the texture BR shows is diffusion and relaxation (Status says so).

    The integrator is the same ROS2 (Chemistry.h) the harness's references use,
    cell by cell, on worker threads; the result is the same for any thread
    count (cells are independent within a substep; the diffusion step reads the
    previous substep's values).
*/
namespace honeydew
{
class BrEngine
{
public:
	/// The grid is small: at most this many cells across; rows follow the aspect.
	static constexpr int kMaxCols = 32;

	void Reset( int cols, int rows, const chem::Recipe& recipe, bool flow, uint32_t salt );
	/// Advance by `chemSeconds` (bounded by the caller), with the eddy
	/// diffusivity (mm^2/s), the relaxation rate (1/s) and the cell size (mm).
	/// Returns the number of ROS2 steps taken over all cells.
	long long Advance( double chemSeconds, double cellMm, double eddyMm2PerS, double relaxPerS, int threads );
	/// A drop of iodide (the perturbation a BR demonstrator adds) in a disc.
	void Drop( double cx, double cy, double radiusCells, double iodideM );

	int Cols() const
	{
		return cols;
	}
	int Rows() const
	{
		return rows;
	}
	/// The species texture, RGBA32F rows bottom-up: ( [I2], [I-], [HOI], [MA] / [MA]0 ).
	const std::vector< float >& Texture() const
	{
		return texture;
	}
	/// Every cell's ten concentrations (for the harness), row-major.
	const std::vector< double >& Cells() const
	{
		return cells;
	}
	double MeanOf( int species ) const;
	double H() const
	{
		return acid;
	}
	bool Flow() const
	{
		return flow;
	}
	long long StepsTaken() const
	{
		return stepsTaken;
	}
	/// --briggs' negative control: the malonic acid step's constant scaled.
	void SetRateScaleForTest( double scale )
	{
		rateScale = scale;
	}

private:
	int cols = 0, rows = 0;
	double acid = 0.0;
	bool flow   = false;
	double feed[ chem::kBRSpecies ] = {};
	std::vector< double > cells, scratch;
	std::vector< float > texture;
	long long stepsTaken = 0;
	double rateScale     = 1.0;
};

} // namespace honeydew
