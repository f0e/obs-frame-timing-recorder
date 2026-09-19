// the "frame timing probe" filter: goes on the game capture source and records when obs's draw of it really
// ran on the gpu, which is when the captured picture was read, plus a fingerprint of the picture it read

#include "probe.hpp"
#include "fingerprint.hpp"
#include "game.hpp"
#include "gpu_timer.hpp"
#include "logs.hpp"
#include "win.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <d3d11_4.h>
#include <wrl/client.h>

#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ft::probe {

	namespace {

		constexpr const char* SETTING_FINGERPRINT = "fingerprint";

		// waiting on this many events at once, the wake event included, is the most WaitForMultipleObjects takes
		constexpr size_t MAX_WAITING = MAXIMUM_WAIT_OBJECTS - 1;

		// what the filter renders into, and so what the output is drawn from, when it's fingerprinting
		constexpr gs_color_space PREFERRED_SPACES[] = { GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED };

		// hands out events for Flush1 and timestamps them from a thread of its own as the gpu sets them, so the
		// graphics thread never waits on the gpu
		class Completion {
		public:
			void start() {
				wake.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
				for (size_t i = 0; i < MAX_WAITING; i++) {
					events.emplace_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
					spare.push_back(events.back().get());
				}
				worker = std::jthread([this](std::stop_token stop) {
					run(stop);
				});
			}

			void stop() {
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
			std::optional<HANDLE> take() {
				std::lock_guard lock(mutex);
				if (spare.empty())
					return std::nullopt;

				HANDLE event = spare.back();
				spare.pop_back();
				return event;
			}

			void submit(uint64_t sequence, HANDLE event) {
				{
					std::lock_guard lock(mutex);
					incoming.push_back({ sequence, event });
				}
				SetEvent(wake.get());
			}

		private:
			struct Waiting {
				uint64_t sequence;
				HANDLE event;
			};

			void run(std::stop_token stop) {
				std::vector<Waiting> waiting;
				std::vector<HANDLE> handles;

				while (!stop.stop_requested()) {
					handles.assign(1, wake.get());
					for (const Waiting& entry : waiting)
						handles.push_back(entry.event);

					DWORD result = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, INFINITE);
					int64_t now = win::qpc_now();

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

					logs::read.update(done.sequence, [now](ReadRecord& record) {
						record.done_qpc = now;
					});

					std::lock_guard lock(mutex);
					spare.push_back(done.event);
				}

				std::lock_guard lock(mutex);
				for (const Waiting& entry : waiting)
					spare.push_back(entry.event);
			}

			std::mutex mutex;
			std::vector<win::Handle> events; // owns them all, spare and in use alike
			std::vector<HANDLE> spare;
			std::vector<Waiting> incoming;
			win::Handle wake;
			std::jthread worker;
		};

		Completion completion;

		struct Probe {
			obs_source_t* context;
			ComPtr<ID3D11DeviceContext3> device_context;
			ID3D11Device* device = nullptr;
			bool unsupported = false;
			bool watching = false;
			bool fingerprinting = true;
			uint64_t last_frame_time = UINT64_MAX;
			Fingerprint fingerprint;
			GpuTimer gpu_timer;

			ID3D11DeviceContext3* ready_context() {
				if (device_context || unsupported)
					return device_context.Get();

				unsupported = true;
				if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
					obs_log(LOG_WARNING, "the probe needs obs's direct3d 11 renderer");
					return nullptr;
				}

				device = static_cast<ID3D11Device*>(gs_get_device_obj());

				ComPtr<ID3D11DeviceContext> immediate;
				device->GetImmediateContext(&immediate);
				if (FAILED(immediate.As(&device_context))) {
					obs_log(LOG_WARNING, "the probe needs direct3d 11.3");
					return nullptr;
				}

				unsupported = false;
				return device_context.Get();
			}

			// the size the source renders at, or nothing if it isn't rendering anything
			std::optional<std::pair<uint32_t, uint32_t>> target_size() const {
				obs_source_t* target = obs_filter_get_target(context);
				if (!target)
					return std::nullopt;

				uint32_t cx = obs_source_get_base_width(target);
				uint32_t cy = obs_source_get_base_height(target);
				if (!cx || !cy)
					return std::nullopt;

				return std::pair{ cx, cy };
			}

			gs_color_space target_space() const {
				return obs_source_get_color_space(
					obs_filter_get_target(context), OBS_COUNTOF(PREFERRED_SPACES), PREFERRED_SPACES
				);
			}
		};

		// the source this filter is on is the one obs captures the game with, so it says which game to log
		void probe_tick(void* data, float) {
			auto* probe = static_cast<Probe*>(data);
			if (probe->watching)
				return;

			if (obs_source_t* capture = obs_filter_get_parent(probe->context)) {
				captured_game.watch(capture);
				probe->watching = true;
			}
		}

		// draws what the fingerprint captured, the way obs draws a filter's own texture
		void draw_picture(gs_texture_t* picture, uint32_t cx, uint32_t cy) {
			gs_effect_t* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			bool srgb = gs_framebuffer_srgb_enabled();

			gs_enable_framebuffer_srgb(true);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), picture);

			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(picture, 0, cx, cy);

			gs_enable_framebuffer_srgb(srgb);
		}

		void probe_render(void* data, gs_effect_t*) {
			auto* probe = static_cast<Probe*>(data);
			uint64_t frame_time = obs_get_video_frame_time();

			// the output's render comes first in each pass - later renders the same tick are previews and projectors
			ID3D11DeviceContext3* context = frame_time == probe->last_frame_time ? nullptr : probe->ready_context();
			std::optional<HANDLE> event = context ? completion.take() : std::nullopt;
			if (!event) {
				obs_source_skip_video_filter(probe->context);
				return;
			}
			probe->last_frame_time = frame_time;

			uint64_t sequence = logs::read.push({ frame_time, win::qpc_now(), 0, 0, 0, 0, 0 });

			// the gpu stamps either side of the draw below, which measures the draw itself rather than the
			// draw plus however long this machine took to wake a thread on the flush
			bool timed = probe->gpu_timer.begin(probe->device, context, sequence);

			// the fingerprint has to be of the picture that was encoded, not of a second read of the shared
			// texture, so the output is drawn from the one texture the fingerprint hashes
			std::optional<std::pair<uint32_t, uint32_t>> size =
				probe->fingerprinting ? probe->target_size() : std::nullopt;
			gs_texture_t* picture =
				size ? probe->fingerprint.capture(probe->context, size->first, size->second, probe->target_space())
				     : nullptr;

			// without one, the pass-through draw is the read instead
			if (!picture)
				obs_source_skip_video_filter(probe->context);

			if (timed)
				probe->gpu_timer.end(context);

			// the event fires once the gpu has run everything flushed up to here, the draw that read the
			// picture included and nothing after it
			context->Flush1(D3D11_CONTEXT_TYPE_ALL, *event);
			completion.submit(sequence, *event);

			GpuTimer::Sample stamped{};
			if (probe->gpu_timer.collect(context, stamped)) {
				logs::read.update(stamped.sequence, [&stamped](ReadRecord& record) {
					record.gpu_begin = stamped.begin;
					record.gpu_end = stamped.end;
					record.gpu_frequency = stamped.frequency;
				});
			}

			if (picture) {
				draw_picture(picture, size->first, size->second);

				probe->fingerprint.hash_older([](uint64_t of, uint64_t hash) {
					logs::read.update(of, [hash](ReadRecord& record) {
						record.fingerprint = hash;
					});
				});
				probe->fingerprint.reduce(sequence);
			}
		}

	} // namespace

	void register_source() {
		static const obs_source_info info{
			.id = "frame_timing_probe",
			.type = OBS_SOURCE_TYPE_FILTER,
			.output_flags = OBS_SOURCE_VIDEO,
			.get_name =
				[](void*) {
					return obs_module_text("Probe.Name");
				},
			.create = [](obs_data_t* settings, obs_source_t* context) -> void* {
				auto* probe = new Probe{ context };
				obs_source_update(context, settings);
				return probe;
			},
			.destroy =
				[](void* data) {
					auto* probe = static_cast<Probe*>(data);
					if (probe->watching)
						captured_game.unwatch();

					obs_enter_graphics();
					probe->fingerprint.release();
					probe->gpu_timer.release();
					obs_leave_graphics();

					delete probe;
				},
			.get_defaults =
				[](obs_data_t* settings) {
					obs_data_set_default_bool(settings, SETTING_FINGERPRINT, true);
				},
			.get_properties =
				[](void*) {
					obs_properties_t* properties = obs_properties_create();
					obs_properties_add_bool(
						properties, SETTING_FINGERPRINT, obs_module_text("Probe.Fingerprint")
					);
					return properties;
				},
			.update =
				[](void* data, obs_data_t* settings) {
					auto* probe = static_cast<Probe*>(data);
					probe->fingerprinting = obs_data_get_bool(settings, SETTING_FINGERPRINT);
				},
			.video_tick = probe_tick,
			.video_render = probe_render,
			.video_get_color_space =
				[](void* data, size_t count, const gs_color_space* preferred) {
					auto* probe = static_cast<Probe*>(data);
					gs_color_space space = probe->target_space();
					for (size_t i = 0; i < count; i++) {
						if (preferred[i] == space)
							return space;
					}
					return count > 0 ? preferred[0] : GS_CS_SRGB;
				},
		};
		obs_register_source(&info);
	}

	void start_worker() {
		completion.start();
	}

	void stop_worker() {
		completion.stop();
	}

} // namespace ft::probe
