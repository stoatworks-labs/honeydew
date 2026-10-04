#pragma once

#include <algorithm>
#include <cmath>

/**
    Chemical time, from the host's clock. No GL, so `hdtest --offline` checks it.

    The host's elapsed time is taken frame to frame in DOUBLE (conway's
    lesson: Resolume's clock has been measured at ~499 million ms, where a
    float resolves 32 ms and the per-frame dt is 0 or 32 ms), bounded (a
    jump backwards, or over a second, passes no time: a clip trigger or a
    scrub), and multiplied by Time-lapse into chemical seconds. The plugin
    then plans substeps for that much chemistry; what the substep cap cannot
    cover is DROPPED and counted, never banked (a stalled host does not earn a
    burst), so the chemistry falls behind real time rather than going
    unstable.
*/
namespace honeydew
{
class ChemicalClock
{
public:
	/// Host seconds. A bigger forward step, or any backward one, is a jump.
	static constexpr double kMaxFrameSeconds = 1.0;

	/// Feed the frame's host time (seconds, unit already settled). Returns the
	/// frame's host dt (0 across a jump).
	double Advance( double hostSeconds )
	{
		double dt = 0.0;
		if( lastHost >= 0.0 )
		{
			const double step = hostSeconds - lastHost;
			if( step >= 0.0 && step <= kMaxFrameSeconds )
				dt = step;
			else
				jumped = true;
		}
		lastHost = hostSeconds;
		return dt;
	}
	bool TakeJump()
	{
		const bool j = jumped;
		jumped       = false;
		return j;
	}
	void Reset()
	{
		lastHost = -1.0;
		jumped   = false;
	}

	/// How many substeps a frame of `chemSeconds` needs at a stable substep of
	/// `maxSubstep` seconds, at most `cap`; the chemical time actually covered
	/// goes to `covered`.
	static int Plan( double chemSeconds, double maxSubstep, int cap, double& covered, double& substep )
	{
		if( chemSeconds <= 0.0 || maxSubstep <= 0.0 )
		{
			covered = 0.0;
			substep = 0.0;
			return 0;
		}
		int n = static_cast< int >( std::ceil( chemSeconds / maxSubstep - 1e-9 ) );
		n     = std::max( n, 1 );
		if( n > cap )
		{
			n       = cap;
			substep = maxSubstep;
			covered = n * maxSubstep;
		}
		else
		{
			substep = chemSeconds / n;
			covered = chemSeconds;
		}
		return n;
	}

private:
	double lastHost = -1.0;
	bool jumped     = false;
};

} // namespace honeydew
