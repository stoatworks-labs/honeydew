#include "BrEngine.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace honeydew
{
using namespace chem;
/// The engine's working tolerance; the harness compares the period it gives
/// with a 1e-8 reference (--briggs).
constexpr double kWorkingRtol = 1e-4;

void BrEngine::Reset( int c, int r, const Recipe& recipe, bool isFlow, uint32_t salt )
{
	cols = std::max( 1, std::min( c, kMaxCols ) );
	rows = std::max( 1, r );
	flow = isFlow;
	double y[ kBRSpecies ], H2O2;
	BRInitialState( recipe, y, acid, H2O2 );
	for( int i = 0; i < kBRSpecies; ++i )
		feed[ i ] = y[ i ];
	cells.assign( static_cast< size_t >( cols ) * rows * kBRSpecies, 0.0 );
	for( int j = 0; j < rows; ++j )
		for( int i = 0; i < cols; ++i )
		{
			double* cell = &cells[ ( static_cast< size_t >( j ) * cols + i ) * kBRSpecies ];
			for( int s = 0; s < kBRSpecies; ++s )
				cell[ s ] = y[ s ];
			//The Seed's heterogeneity: the trace iodide varies by up to a
			//factor of two from cell to cell, so the cells do not fire as one.
			const double h = CellHash( i, j, salt ) * ( 1.0 / 4294967296.0 );
			cell[ BR_I ] *= 0.5 + h;
		}
	texture.assign( static_cast< size_t >( cols ) * rows * 4, 0.0f );
	stepsTaken = 0;
}

double BrEngine::MeanOf( int species ) const
{
	if( cells.empty() )
		return 0.0;
	double sum = 0.0;
	const size_t n = static_cast< size_t >( cols ) * rows;
	for( size_t k = 0; k < n; ++k )
		sum += cells[ k * kBRSpecies + species ];
	return sum / static_cast< double >( n );
}

void BrEngine::Drop( double cx, double cy, double radiusCells, double iodideM )
{
	for( int j = 0; j < rows; ++j )
		for( int i = 0; i < cols; ++i )
		{
			const double dx = i + 0.5 - cx, dy = j + 0.5 - cy;
			if( dx * dx + dy * dy <= radiusCells * radiusCells )
				cells[ ( static_cast< size_t >( j ) * cols + i ) * kBRSpecies + BR_I ] += iodideM;
		}
}

long long BrEngine::Advance( double chemSeconds, double cellMm, double eddy, double relax, int threads )
{
	if( cells.empty() || chemSeconds <= 0.0 )
		return 0;
	const size_t n = static_cast< size_t >( cols ) * rows;
	//Diffusion first (explicit, five-point, no-flux), on the substep that
	//keeps it stable; the reaction then runs each cell over the whole
	//interval with its own adaptive steps.
	const double D = kBR.DAll * kCm2PerS_to_Mm2PerS + eddy;
	const double h2 = cellMm * cellMm;
	const int sub   = std::max( 1, static_cast< int >( std::ceil( chemSeconds * D / ( 0.2 * h2 ) ) ) );
	const double dt = chemSeconds / sub;
	if( scratch.size() != cells.size() )
		scratch.resize( cells.size() );
	for( int k = 0; k < sub && n > 1; ++k )
	{
		for( int j = 0; j < rows; ++j )
			for( int i = 0; i < cols; ++i )
			{
				const size_t at = static_cast< size_t >( j ) * cols + i;
				const size_t l = static_cast< size_t >( j ) * cols + std::max( i - 1, 0 ), r = static_cast< size_t >( j ) * cols + std::min( i + 1, cols - 1 );
				const size_t d = static_cast< size_t >( std::max( j - 1, 0 ) ) * cols + i, u = static_cast< size_t >( std::min( j + 1, rows - 1 ) ) * cols + i;
				for( int s = 0; s < kBRSpecies; ++s )
				{
					const double c   = cells[ at * kBRSpecies + s ];
					const double lap = ( cells[ l * kBRSpecies + s ] + cells[ r * kBRSpecies + s ] + cells[ d * kBRSpecies + s ] + cells[ u * kBRSpecies + s ] - 4.0 * c ) / h2;
					scratch[ at * kBRSpecies + s ] = c + dt * D * lap;
				}
			}
		std::swap( cells, scratch );
	}
	//The whole-vessel relaxation, exact over the interval.
	if( relax > 0.0 && n > 1 )
	{
		const double a = 1.0 - std::exp( -relax * chemSeconds );
		for( int s = 0; s < kBRSpecies; ++s )
		{
			const double mean = MeanOf( s );
			for( size_t k = 0; k < n; ++k )
				cells[ k * kBRSpecies + s ] += a * ( mean - cells[ k * kBRSpecies + s ] );
		}
	}
	//The reaction, cell by cell, on worker threads.
	const int workers = std::clamp( threads, 1, 8 );
	std::vector< long long > counts( static_cast< size_t >( workers ), 0 );
	auto work = [ & ]( int w ) {
		for( size_t k = static_cast< size_t >( w ); k < n; k += static_cast< size_t >( workers ) )
		{
			double* y   = &cells[ k * kBRSpecies ];
			BRKinetics kk = kBR;
			kk.r9 *= rateScale;
			const RhsFn rhs = [ & ]( const double* yy, double* dy ) { BRRhs( kk, acid, flow ? kBR.k0CSTR : 0.0, flow ? feed : nullptr, yy, dy ); };
			const StiffResult r = IntegrateStiff( kBRSpecies, rhs, y, chemSeconds, kWorkingRtol, 1e-15, nullptr, 0.0 );
			for( int s = 0; s < kBRSpecies; ++s )
				y[ s ] = std::max( y[ s ], 0.0 );
			counts[ static_cast< size_t >( w ) ] += r.steps;
		}
	};
	if( workers == 1 )
		work( 0 );
	else
	{
		std::vector< std::thread > pool;
		for( int w = 0; w < workers; ++w )
			pool.emplace_back( work, w );
		for( std::thread& t : pool )
			t.join();
	}
	long long total = 0;
	for( long long c : counts )
		total += c;
	stepsTaken += total;
	//The texture the colour pass reads.
	const double ma0 = feed[ BR_MA ] > 0.0 ? feed[ BR_MA ] : 1.0;
	for( size_t k = 0; k < n; ++k )
	{
		const double* y = &cells[ k * kBRSpecies ];
		texture[ k * 4 + 0 ] = static_cast< float >( y[ BR_I2 ] );
		texture[ k * 4 + 1 ] = static_cast< float >( y[ BR_I ] );
		texture[ k * 4 + 2 ] = static_cast< float >( y[ BR_HOI ] );
		texture[ k * 4 + 3 ] = static_cast< float >( y[ BR_MA ] / ma0 );
	}
	return total;
}

} // namespace honeydew
