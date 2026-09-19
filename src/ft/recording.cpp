// logs for recordings. a recording can run for hours, far longer than the logs are kept in memory, so its
// sidecar is written as it goes: opened beside the recording, appended to every second, and finished when the
// recording stops or splits into a new file

#include "recording.hpp"
#include "game.hpp"
#include "logs.hpp"
#include "sidecar.hpp"
#include "win.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/util.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <thread>

namespace ft::recording {

	namespace {

		using namespace std::chrono_literals;

		constexpr auto COPY_INTERVAL = 1s;

		// logged from this long before a file starts, so its first frames have the game frames before them
		constexpr auto LEAD = 5s;

		// a read is left for this long before it's written out, so the gpu has reported it
		constexpr auto READ_SETTLE = 1s;

		// the log for one recorded file, from when it started until it's finished
		class Segment {
		public:
			Segment(std::string video, uint64_t first_packet, obs_output_t* output)
				: video(std::move(video)), started(win::qpc_now() - win::qpc_ticks(LEAD)), packets(first_packet),
				  file(sidecar::open(sidecar_path(), started, output)) {
				if (!file)
					obs_log(LOG_WARNING, "couldn't write %s", sidecar_path().c_str());
			}

			std::string sidecar_path() const {
				return video + std::string(sidecar::SUFFIX);
			}

			void write_new(bool finishing) {
				if (!file)
					return;

				int64_t settled = win::qpc_now() - (finishing ? 0 : win::qpc_ticks(READ_SETTLE));
				int64_t from = started;

				append("TICK", logs::tick, ticks, anything, [from](const TickRecord& r) {
					return r.qpc >= from;
				});
				append(
					"READ",
					logs::read,
					reads,
					[settled](const ReadRecord& r) {
						return r.submitted_qpc < settled;
					},
					[from](const ReadRecord& r) {
						return r.submitted_qpc >= from;
					}
				);
				append("PCKT", logs::recording_packets.records, packets, anything, anything);
				append("PRES", logs::present, presents, anything, [from](const PresentRecord& r) {
					return (int64_t)r.present_start >= from;
				});
			}

			bool finish(int64_t saved) {
				if (!file)
					return false;

				write_new(true);
				sidecar::write_batch(file, "GAME", captured_game.records());
				sidecar::set_saved_qpc(file, saved);

				file.close();
				return file.good();
			}

		private:
			template<typename T>
			void append(
				std::string_view tag,
				const Ring<T>& log,
				uint64_t& cursor,
				std::predicate<const T&> auto&& ready,
				std::predicate<const T&> auto&& keep
			) {
				std::vector<T> records = log.read_from(cursor, ready);
				std::erase_if(records, [&](const T& record) {
					return !keep(record);
				});
				sidecar::write_batch(file, tag, records);
			}

			std::string video;
			int64_t started;
			uint64_t ticks = 0;
			uint64_t reads = 0;
			uint64_t packets;
			uint64_t presents = 0;
			std::ofstream file;
		};

		// how many packets back a split file's log starts, to cover LEAD
		uint64_t lead_packets() {
			obs_video_info video{};
			if (!obs_get_video_info(&video) || !video.fps_den)
				return 0;
			return (uint64_t)LEAD.count() * video.fps_num / video.fps_den;
		}

		std::mutex mutex;
		std::unique_ptr<Segment> current;
		OBSOutput watched;
		uint64_t first_packet = 0; // where the recording's packets start in its packet log
		std::vector<std::jthread> finishers;

		std::jthread copier;
		std::mutex wake_mutex;
		std::condition_variable_any wake;

		// finishes a segment's sidecar away from whichever thread ended it
		void finish(std::unique_ptr<Segment> segment) {
			if (!segment)
				return;

			int64_t saved = win::qpc_now();

			std::lock_guard lock(mutex);
			finishers.emplace_back([segment = std::move(segment), saved]() mutable {
				// the last reads are usually reported by the gpu within a few milliseconds
				std::this_thread::sleep_for(200ms);

				std::string sidecar_file = segment->sidecar_path();
				if (!segment->finish(saved))
					obs_log(LOG_WARNING, "couldn't write %s", sidecar_file.c_str());
				else
					obs_log(LOG_INFO, "wrote %s", sidecar_file.c_str());
			});
		}

		void on_file_changed(void*, calldata_t* params) {
			const char* next = calldata_string(params, "next_file");
			if (!next)
				return;

			std::unique_ptr<Segment> done;
			{
				std::lock_guard lock(mutex);
				done = std::move(current);
				uint64_t written = logs::recording_packets.records.written();
				uint64_t lead = std::min(written, lead_packets());
				current = std::make_unique<Segment>(next, std::max(first_packet, written - lead), watched.Get());
			}
			finish(std::move(done));
		}

		void copy_loop(std::stop_token stop) {
			while (!stop.stop_requested()) {
				std::unique_lock waking(wake_mutex);
				wake.wait_for(waking, stop, COPY_INTERVAL, [&stop] {
					return stop.stop_requested();
				});
				waking.unlock();

				std::lock_guard lock(mutex);
				if (current)
					current->write_new(false);
			}
		}

		void unwatch() {
			if (!watched)
				return;

			signal_handler_disconnect(obs_output_get_signal_handler(watched), "file_changed", on_file_changed, nullptr);
			watched = nullptr;
		}

	} // namespace

	void starting() {
		OBSOutputAutoRelease output = obs_frontend_get_recording_output();
		if (!output)
			return;

		std::lock_guard lock(mutex);
		unwatch();
		watched = output.Get();
		signal_handler_connect(obs_output_get_signal_handler(watched), "file_changed", on_file_changed, nullptr);

		// packets can arrive before the started event is handled, so the log is attached already
		logs::recording_packets.attach(output);
		first_packet = logs::recording_packets.records.written();

		if (!copier.joinable())
			copier = std::jthread(copy_loop);
	}

	void started() {
		BPtr<char> path = obs_frontend_get_last_recording();
		if (!path)
			return;

		std::unique_ptr<Segment> previous;
		{
			std::lock_guard lock(mutex);
			previous = std::move(current);
			current = std::make_unique<Segment>(path.Get(), first_packet, watched.Get());
		}
		finish(std::move(previous));
	}

	void stopped() {
		std::unique_ptr<Segment> done;
		{
			std::lock_guard lock(mutex);
			done = std::move(current);
			unwatch();
		}
		finish(std::move(done));
		logs::recording_packets.detach();
	}

	void finish_all() {
		stopped();

		// a jthread stops and joins when it's assigned over or destroyed
		copier = {};

		std::vector<std::jthread> waiting;
		{
			std::lock_guard lock(mutex);
			waiting.swap(finishers);
		}
		waiting.clear();
	}

} // namespace ft::recording
