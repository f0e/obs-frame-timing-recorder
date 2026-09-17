// stand-in for presentmon's precision waiter. presentdata only uses it to pace playback of trace files,
// which a live session never does
#pragma once
#include "Qpc.h"

namespace pmon::util
{
	class PrecisionWaiter
	{
	public:
		PrecisionWaiter(double = 0.001) noexcept {}
		PrecisionWaiter(const PrecisionWaiter&) = delete;
		PrecisionWaiter& operator=(const PrecisionWaiter&) = delete;
		double Wait(double seconds, bool = false) noexcept;
	};
}
