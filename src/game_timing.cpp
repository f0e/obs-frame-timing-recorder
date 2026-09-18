// logs when every game frame was started, presented and finished on the gpu, from windows' event tracing - the
// same way presentmon does, with its presentdata library. nothing here touches the game: the events come from
// windows
//
// starting a trace session needs obs to run as administrator, or the user to be in the "Performance Log Users"
// group. without either, everything else the plugin logs still works

#include "game_timing.hpp"
#include "game.hpp"
#include "logs.hpp"
#include "win.hpp"

#include <plugin-support.h>

#include <PresentData/PresentMonTraceConsumer.hpp>
#include <PresentData/PresentMonTraceSession.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace ft {

namespace {

constexpr wchar_t SESSION_NAME[] = L"obs-frame-timing-recorder";

// presents the consumer holds before they're collected. presentmon's default drops presents from games running
// at a few hundred fps
constexpr uint32_t CONSUMER_BUFFER = 16384;

constexpr auto COLLECT_INTERVAL = std::chrono::milliseconds(20);

std::atomic<GameTimingStatus> status = GameTimingStatus::NOT_STARTED;
std::unique_ptr<PMTraceConsumer> consumer;
PMTraceSession session;
std::jthread consume_thread;
std::jthread collect_thread;

PresentRecord record_of(const PresentEvent &present)
{
	return {
		.present_start = present.PresentStartTime,
		.time_in_present = present.TimeInPresent,
		.gpu_start = present.GPUStartTime,
		.ready = present.ReadyTime,
		.gpu_duration = present.GPUDuration,
		.swap_chain = present.SwapChainAddress,
		.process_id = present.ProcessId,
		.runtime = (uint64_t)present.Runtime,
		.present_mode = (uint64_t)present.PresentMode,
		.final_state = (uint64_t)present.FinalState,
		.flags = (present.IsLost ? PRESENT_LOST : 0) | (present.PresentFailed ? PRESENT_FAILED : 0),
		.app_sim_start = present.AppSimStartTime,
		.reflex_sim_start = present.PclSimStartTime,
		.screen_time = present.Displayed.empty() ? 0 : present.Displayed.front().second,
		.window = present.Hwnd,
	};
}

void collect(std::stop_token stop)
{
	std::vector<std::shared_ptr<PresentEvent>> presents;
	std::vector<ProcessEvent> processes;

	while (!stop.stop_requested()) {
		std::this_thread::sleep_for(COLLECT_INTERVAL);

		consumer->DequeueProcessEvents(processes);
		processes.clear();

		// everything windows presents goes past here, and only the captured game can be in a recording
		consumer->DequeuePresentEvents(presents);
		for (const auto &present : presents) {
			if (present && captured_game.presented(present->ProcessId))
				present_log.push(record_of(*present));
		}
		presents.clear();
	}
}

GameTimingStatus fail(GameTimingStatus why)
{
	consumer.reset();
	status = why;
	return why;
}

} // namespace

GameTimingStatus start_game_timing()
{
	consumer = std::make_unique<PMTraceConsumer>(CONSUMER_BUFFER);
	// presentdata hands a present over as soon as it stops tracking it, and without display tracking that's
	// before its gpu work is known
	consumer->mTrackDisplay = true;
	consumer->mTrackGPU = true;
	// games that say when they simulate each frame
	consumer->mTrackAppTiming = true;
	consumer->mTrackPcLatency = true;

	session.mPMConsumer = consumer.get();
	ULONG result = session.Start(nullptr, SESSION_NAME);

	// a session left behind by an obs that didn't shut down cleanly
	if (result == ERROR_ALREADY_EXISTS) {
		obs_log(LOG_INFO, "replacing a trace session left running from before");
		if (StopNamedTraceSession(SESSION_NAME) == ERROR_SUCCESS)
			result = session.Start(nullptr, SESSION_NAME);
	}

	if (result == ERROR_ACCESS_DENIED) {
		obs_log(LOG_WARNING,
			"no permission to trace game frames - run OBS as administrator, or add your account to the "
			"\"Performance Log Users\" group and sign in again (%s). Recordings are still logged, with "
			"less accurate timing",
			in_performance_log_users() ? "you're in the group, but this sign-in session doesn't have it yet"
						   : "you're not in the group");
		return fail(GameTimingStatus::NO_PERMISSION);
	}

	if (result != ERROR_SUCCESS) {
		obs_log(LOG_WARNING, "couldn't start tracing game frames (error %lu)", result);
		return fail(GameTimingStatus::FAILED);
	}

	consume_thread = std::jthread([] {
		SetThreadDescription(GetCurrentThread(), L"frame timing trace");
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		TRACEHANDLE handle = session.mTraceHandle;
		ProcessTrace(&handle, 1, nullptr, nullptr);
	});
	collect_thread = std::jthread(collect);

	obs_log(LOG_INFO, "tracing game frames");
	status = GameTimingStatus::TRACING;
	return status;
}

void stop_game_timing()
{
	if (status != GameTimingStatus::TRACING)
		return;

	// stopping the session ends ProcessTrace; assigning over a jthread asks it to stop and joins it
	session.Stop();
	consume_thread = {};
	collect_thread = {};

	consumer.reset();
	status = GameTimingStatus::NOT_STARTED;
}

GameTimingStatus game_timing_status()
{
	return status;
}

} // namespace ft
