// implementations behind the stand-in headers
#include "PrecisionWaiter.h"
#include "Qpc.h"

#include <windows.h>

namespace pmon::util
{
	int64_t GetCurrentTimestamp() noexcept
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	uint64_t GetTimestampFrequencyUint64() noexcept
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return (uint64_t)frequency.QuadPart;
	}

	double GetTimestampFrequencyDouble() noexcept
	{
		return (double)GetTimestampFrequencyUint64();
	}

	double GetTimestampPeriodSeconds() noexcept
	{
		return 1.0 / GetTimestampFrequencyDouble();
	}

	double TimestampDeltaToSeconds(int64_t start, int64_t end, double period) noexcept
	{
		return double(end - start) * period;
	}

	double PrecisionWaiter::Wait(double seconds, bool) noexcept
	{
		if (seconds > 0)
			Sleep(DWORD(seconds * 1000));
		return seconds;
	}
}
