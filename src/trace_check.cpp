// checks whether this account can trace game frames the way the plugin does, and shows what it sees
//
// usage: frame-timing-check [seconds]

#include "process_names.hpp"
#include "win.hpp"

#include <PresentData/PresentMonTraceConsumer.hpp>
#include <PresentData/PresentMonTraceSession.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <thread>

using namespace ft;

namespace {

constexpr wchar_t SESSION_NAME[] = L"obs-frame-timing-recorder-check";

struct Totals {
	size_t presents = 0;
	size_t with_gpu = 0;
	size_t with_sim = 0;
	uint64_t first = 0;
	uint64_t last = 0;

	void add(const PresentEvent &present)
	{
		presents++;
		with_gpu += present.ReadyTime != 0;
		with_sim += present.AppSimStartTime != 0 || present.PclSimStartTime != 0;
		if (!first)
			first = present.PresentStartTime;
		last = present.PresentStartTime;
	}

	double fps() const
	{
		double span = double(last - first) / double(qpc_frequency());
		return span > 0 ? double(presents - 1) / span : 0;
	}

	double percent(size_t part) const { return presents ? 100.0 * double(part) / double(presents) : 0; }
};

std::map<uint32_t, Totals> trace(PMTraceConsumer &consumer, std::chrono::seconds seconds)
{
	std::map<uint32_t, Totals> totals;
	std::vector<std::shared_ptr<PresentEvent>> presents;

	for (auto until = std::chrono::steady_clock::now() + seconds; std::chrono::steady_clock::now() < until;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		consumer.DequeuePresentEvents(presents);
		for (const auto &present : presents) {
			if (present)
				totals[present->ProcessId].add(*present);
		}
		presents.clear();
	}
	return totals;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	auto seconds = std::chrono::seconds(argc > 1 ? _wtoi(argv[1]) : 5);

	wprintf(L"running as administrator: %s\n", running_elevated() ? L"yes" : L"no");
	wprintf(L"in \"Performance Log Users\" (this sign-in): %s\n", in_performance_log_users() ? L"yes" : L"no");

	PMTraceConsumer consumer(16384);
	consumer.mTrackDisplay = true;
	consumer.mTrackGPU = true;
	consumer.mTrackAppTiming = true;
	consumer.mTrackPcLatency = true;

	PMTraceSession session;
	session.mPMConsumer = &consumer;
	ULONG result = session.Start(nullptr, SESSION_NAME);
	if (result == ERROR_ALREADY_EXISTS && StopNamedTraceSession(SESSION_NAME) == ERROR_SUCCESS)
		result = session.Start(nullptr, SESSION_NAME);

	if (result == ERROR_ACCESS_DENIED) {
		wprintf(L"\ncan't trace game frames: access denied. run as administrator, or join \"Performance Log "
			L"Users\" and sign in again\n");
		return 1;
	}
	if (result != ERROR_SUCCESS) {
		wprintf(L"\ncan't trace game frames: error %lu\n", result);
		return 1;
	}

	wprintf(L"\ntracing for %lld seconds...\n", (long long)seconds.count());
	std::jthread consume([&] {
		TRACEHANDLE handle = session.mTraceHandle;
		ProcessTrace(&handle, 1, nullptr, nullptr);
	});

	std::map<uint32_t, Totals> totals = trace(consumer, seconds);
	session.Stop();

	ProcessNames names;
	wprintf(L"\n%-32s %10s %8s %10s %15s\n", L"process", L"presents", L"fps", L"gpu timed", L"sim reported");
	for (const auto &[process_id, totals_of] : totals) {
		names.learn(process_id);
		std::string name = names.name(process_id);
		wprintf(L"%-32s %10zu %8.0f %9.0f%% %14.0f%%\n", widen(name.empty() ? "?" : name).c_str(),
			totals_of.presents, totals_of.fps(), totals_of.percent(totals_of.with_gpu),
			totals_of.percent(totals_of.with_sim));
	}
	return 0;
}
