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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace ft::game_timing {

	namespace {

		constexpr wchar_t SESSION_NAME[] = L"obs-frame-timing-recorder";

		// what ControlTrace wants: the properties with room for the session name after them
		struct TraceProperties : EVENT_TRACE_PROPERTIES {
			wchar_t session_name[MAX_PATH];
		};

		// presents the consumer holds before they're collected. presentmon's default drops presents from games running
		// at a few hundred fps
		constexpr uint32_t CONSUMER_BUFFER = 16384;

		constexpr auto COLLECT_INTERVAL = std::chrono::milliseconds(20);

		// a flush is only a request, and presentdata holds a present back until it knows how it ended, so the
		// wait is on the presents arriving rather than on the flush returning
		constexpr auto DRAIN_POLL = std::chrono::milliseconds(5);
		constexpr auto REFLUSH_INTERVAL = std::chrono::milliseconds(250);

		std::atomic<Status> current = Status::NOT_STARTED;
		// the newest present handed to the log, so a drain can tell how far the trace has got
		std::atomic<int64_t> collected = 0;
		std::unique_ptr<PMTraceConsumer> consumer;
		PMTraceSession session;
		std::jthread consume_thread;
		std::jthread collect_thread;

		PresentRecord record_of(const PresentEvent& present) {
			return {
				.present_start = present.PresentStartTime,
				.time_in_present = present.TimeInPresent,
				.gpu_start = present.GPUStartTime,
				.ready = present.ReadyTime,
				.swap_chain = present.SwapChainAddress,
				.process_id = present.ProcessId,
				.runtime = (uint64_t)present.Runtime,
				.present_mode = (uint64_t)present.PresentMode,
				.final_state = (uint64_t)present.FinalState,
				.flags = (present.IsLost ? PRESENT_LOST : 0) | (present.PresentFailed ? PRESENT_FAILED : 0),
				.app_sim_start = present.AppSimStartTime,
				.app_sim_end = present.AppSimEndTime,
				.reflex_sim_start = present.PclSimStartTime,
				.reflex_sim_end = present.PclSimEndTime,
				.screen_time = present.Displayed.empty() ? 0 : present.Displayed.front().second,
				.frame_type = present.Displayed.empty() ? 0 : (uint64_t)present.Displayed.front().first,
				.window = present.Hwnd,
			};
		}

		void collect(std::stop_token stop) {
			std::vector<std::shared_ptr<PresentEvent>> presents;
			std::vector<ProcessEvent> processes;

			while (!stop.stop_requested()) {
				std::this_thread::sleep_for(COLLECT_INTERVAL);

				consumer->DequeueProcessEvents(processes);
				processes.clear();

				// everything windows presents goes past here, and only the captured game can be in a recording
				consumer->DequeuePresentEvents(presents);
				int64_t newest = collected.load(std::memory_order_relaxed);
				for (const auto& present : presents) {
					if (present && captured_game.presented(present->ProcessId)) {
						logs::present.push(record_of(*present));
						// presentdata completes a present once it knows how it ended, which is roughly but not
						// exactly the order they started in
						newest = std::max(newest, (int64_t)present->PresentStartTime);
					}
				}
				collected.store(newest, std::memory_order_relaxed);
				presents.clear();
			}
		}

		Status fail(Status why) {
			consumer.reset();
			current = why;
			return why;
		}

	} // namespace

	Status start() {
		consumer = std::make_unique<PMTraceConsumer>(CONSUMER_BUFFER);
		// presentdata hands a present over as soon as it stops tracking it, and without display tracking that's
		// before its gpu work is known
		consumer->mTrackDisplay = true;
		consumer->mTrackGPU = true;
		// games that say when they simulate each frame
		consumer->mTrackAppTiming = true;
		consumer->mTrackPcLatency = true;
		// what the compositor showed: the game's own frame, or one generated between two of them
		consumer->mTrackFrameType = true;

		session.mPMConsumer = consumer.get();
		ULONG result = session.Start(nullptr, SESSION_NAME);

		// a session left behind by an obs that didn't shut down cleanly
		if (result == ERROR_ALREADY_EXISTS) {
			obs_log(LOG_INFO, "replacing a trace session left running from before");
			if (StopNamedTraceSession(SESSION_NAME) == ERROR_SUCCESS)
				result = session.Start(nullptr, SESSION_NAME);
		}

		if (result == ERROR_ACCESS_DENIED) {
			obs_log(
				LOG_WARNING,
				"no permission to trace game frames - run OBS as administrator, or add your account to the "
				"\"Performance Log Users\" group and sign in again (%s). Recordings are still logged, with "
				"less accurate timing",
				win::in_performance_log_users() ? "you're in the group, but this sign-in session doesn't have it yet"
												: "you're not in the group"
			);
			return fail(Status::NO_PERMISSION);
		}

		if (result != ERROR_SUCCESS) {
			obs_log(LOG_WARNING, "couldn't start tracing game frames (error %lu)", result);
			return fail(Status::FAILED);
		}

		consume_thread = std::jthread([] {
			SetThreadDescription(GetCurrentThread(), L"frame timing trace");
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			TRACEHANDLE handle = session.mTraceHandle;
			ProcessTrace(&handle, 1, nullptr, nullptr);
		});
		collect_thread = std::jthread(collect);

		obs_log(LOG_INFO, "tracing game frames");
		current = Status::TRACING;
		return current;
	}

	bool drain(int64_t until, std::chrono::milliseconds give_up_after) {
		if (current != Status::TRACING)
			return true;

		auto flush = [] {
			// only the session name and the size have to be filled in for a flush
			TraceProperties properties = {};
			properties.Wnode.BufferSize = sizeof(properties);
			properties.LoggerNameOffset = offsetof(TraceProperties, session_name);
			ControlTraceW(session.mSessionHandle, nullptr, &properties, EVENT_TRACE_CONTROL_FLUSH);
		};

		auto deadline = std::chrono::steady_clock::now() + give_up_after;
		auto next_flush = std::chrono::steady_clock::now();

		while (collected.load(std::memory_order_relaxed) < until) {
			auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				return false;
			// the game keeps presenting while this waits, so one flush only covers what was buffered then
			if (now >= next_flush) {
				flush();
				next_flush = now + REFLUSH_INTERVAL;
			}
			std::this_thread::sleep_for(DRAIN_POLL);
		}

		return true;
	}

	void stop() {
		if (current != Status::TRACING)
			return;

		// stopping the session ends ProcessTrace; assigning over a jthread asks it to stop and joins it
		session.Stop();
		consume_thread = {};
		collect_thread = {};

		consumer.reset();
		current = Status::NOT_STARTED;
	}

	Status status() {
		return current;
	}

} // namespace ft::game_timing
