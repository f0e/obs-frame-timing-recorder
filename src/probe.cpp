// the "frame timing probe" filter: goes on the game capture source and records when obs's draw of it really
// ran on the gpu, which is when the captured picture was read

#include "probe.hpp"
#include "game.hpp"
#include "logs.hpp"
#include "win.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <d3d11_4.h>
#include <wrl/client.h>

#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ft {

namespace {

// waiting on this many events at once, the wake event included, is the most WaitForMultipleObjects takes
constexpr size_t MAX_WAITING = MAXIMUM_WAIT_OBJECTS - 1;

// hands out events for Flush1 and timestamps them from a thread of its own as the gpu sets them, so the
// graphics thread never waits on the gpu
class Completion {
public:
	void start()
	{
		wake.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		for (size_t i = 0; i < MAX_WAITING; i++) {
			events.emplace_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
			spare.push_back(events.back().get());
		}
		worker = std::jthread([this](std::stop_token stop) { run(stop); });
	}

	void stop()
	{
		if (!worker.joinable())
			return;

		worker.request_stop();
		SetEvent(wake.get());
		worker.join();

		spare.clear();
		incoming.clear();
		events.clear();
		wake.reset();
	}

	// nothing when every event is still out, in which case this frame goes unmeasured
	std::optional<HANDLE> take()
	{
		std::lock_guard lock(mutex);
		if (spare.empty())
			return std::nullopt;

		HANDLE event = spare.back();
		spare.pop_back();
		return event;
	}

	void submit(uint64_t sequence, HANDLE event)
	{
		{
			std::lock_guard lock(mutex);
			incoming.push_back({sequence, event});
		}
		SetEvent(wake.get());
	}

private:
	struct Waiting {
		uint64_t sequence;
		HANDLE event;
	};

	void run(std::stop_token stop)
	{
		std::vector<Waiting> waiting;
		std::vector<HANDLE> handles;

		while (!stop.stop_requested()) {
			handles.assign(1, wake.get());
			for (const Waiting &entry : waiting)
				handles.push_back(entry.event);

			DWORD result = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, INFINITE);
			int64_t now = qpc_now();

			if (result == WAIT_OBJECT_0) {
				std::lock_guard lock(mutex);
				waiting.insert(waiting.end(), incoming.begin(), incoming.end());
				incoming.clear();
				continue;
			}

			size_t index = result - WAIT_OBJECT_0 - 1;
			if (index >= waiting.size())
				continue;

			Waiting done = waiting[index];
			waiting.erase(waiting.begin() + index);

			read_log.update(done.sequence, [now](ReadRecord &record) { record.done_qpc = now; });

			std::lock_guard lock(mutex);
			spare.push_back(done.event);
		}

		std::lock_guard lock(mutex);
		for (const Waiting &entry : waiting)
			spare.push_back(entry.event);
	}

	std::mutex mutex;
	std::vector<Handle> events; // owns them all, spare and in use alike
	std::vector<HANDLE> spare;
	std::vector<Waiting> incoming;
	Handle wake;
	std::jthread worker;
};

Completion completion;

struct Probe {
	obs_source_t *context;
	ComPtr<ID3D11DeviceContext3> device_context;
	bool unsupported = false;
	bool watching = false;
	uint64_t last_frame_time = UINT64_MAX;

	ID3D11DeviceContext3 *ready_context()
	{
		if (device_context || unsupported)
			return device_context.Get();

		unsupported = true;
		if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
			obs_log(LOG_WARNING, "the probe needs obs's direct3d 11 renderer");
			return nullptr;
		}

		ComPtr<ID3D11DeviceContext> immediate;
		static_cast<ID3D11Device *>(gs_get_device_obj())->GetImmediateContext(&immediate);
		if (FAILED(immediate.As(&device_context))) {
			obs_log(LOG_WARNING, "the probe needs direct3d 11.3");
			return nullptr;
		}

		unsupported = false;
		return device_context.Get();
	}
};

// the source this filter is on is the one obs captures the game with, so it says which game to log
void probe_tick(void *data, float)
{
	auto *probe = static_cast<Probe *>(data);
	if (probe->watching)
		return;

	if (obs_source_t *capture = obs_filter_get_parent(probe->context)) {
		captured_game.watch(capture);
		probe->watching = true;
	}
}

void probe_render(void *data, gs_effect_t *)
{
	auto *probe = static_cast<Probe *>(data);
	uint64_t frame_time = obs_get_video_frame_time();

	// the output's render comes first in each pass - later renders the same tick are previews and projectors
	ID3D11DeviceContext3 *context = frame_time == probe->last_frame_time ? nullptr : probe->ready_context();
	std::optional<HANDLE> event = context ? completion.take() : std::nullopt;
	if (!event) {
		obs_source_skip_video_filter(probe->context);
		return;
	}
	probe->last_frame_time = frame_time;

	obs_source_skip_video_filter(probe->context);

	// the event fires once the gpu has run everything flushed up to here, the draw above included
	context->Flush1(D3D11_CONTEXT_TYPE_ALL, *event);
	uint64_t sequence = read_log.push({frame_time, qpc_now(), 0});
	completion.submit(sequence, *event);
}

} // namespace

void register_probe()
{
	static const obs_source_info info{
		.id = "frame_timing_probe",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = [](void *) { return obs_module_text("Probe.Name"); },
		.create = [](obs_data_t *, obs_source_t *context) -> void * { return new Probe{context}; },
		.destroy =
			[](void *data) {
				auto *probe = static_cast<Probe *>(data);
				if (probe->watching)
					captured_game.unwatch();
				delete probe;
			},
		.video_tick = probe_tick,
		.video_render = probe_render,
	};
	obs_register_source(&info);
}

void start_probe_worker()
{
	completion.start();
}

void stop_probe_worker()
{
	completion.stop();
}

} // namespace ft
