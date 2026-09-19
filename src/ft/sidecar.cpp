#include "sidecar.hpp"
#include "game.hpp"
#include "game_timing.hpp"
#include "logs.hpp"
#include "win.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstddef>

namespace ft::sidecar {

	namespace {

#pragma pack(push, 1)

		struct Header {
			char magic[8];
			uint32_t version;
			uint32_t game_timing; // game_timing::Status
			int64_t qpc_frequency;
			int64_t saved_qpc;
			uint32_t fps_num;
			uint32_t fps_den;
			// how many ticks after the tick whose timestamp a frame carries the frame was rendered
			uint32_t render_delay;
		};

		struct BatchHeader {
			char tag[4];
			uint32_t count;
		};

#pragma pack(pop)

		// blur's frame_timing_log.py unpacks these, so they have to stay as they are
		static_assert(sizeof(Header) == 44);
		static_assert(offsetof(Header, saved_qpc) == 24);
		static_assert(sizeof(BatchHeader) == 8);

		constexpr uint32_t VERSION = 8;

		// Which tick rendered the frame a packet carries, relative to the tick whose timestamp it has.
		//
		// `video_sleep` queues the time it's leaving at the end of a pass (`libobs/obs-video.c`), and the next
		// pass hands that time to whatever it renders. With a texture encoder that happens in `render_video`'s
		// own pass, so the frame was rendered one tick after the tick it's stamped with. A raw encoder instead
		// gets the texture `output_frame` staged in the *previous* pass and downloads it in this one, so its
		// frame was rendered on the tick it's stamped with.
		//
		// Which of the two an output uses is libobs's `gpu_encode_available`, evaluated here through the two
		// public calls it's made of.
		uint32_t render_delay(obs_output_t* output) {
			obs_encoder_t* encoder = output ? obs_output_get_video_encoder(output) : nullptr;
			if (!encoder || !obs_encoder_active(encoder))
				return 1;

			bool texture = (obs_encoder_get_caps(encoder) & OBS_ENCODER_CAP_PASS_TEXTURE) != 0 &&
			               (obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_NV12) ||
			                obs_encoder_video_tex_active(encoder, VIDEO_FORMAT_P010));

			return texture ? 1 : 0;
		}

		template<typename T>
		bool write_record(std::ostream& out, const T& record) {
			out.write(reinterpret_cast<const char*>(&record), sizeof(record));
			return (bool)out;
		}

	} // namespace

	std::ofstream open(std::string_view path, int64_t saved_qpc, obs_output_t* output) {
		std::ofstream file(win::as_path(path), std::ios::binary);
		if (!file)
			return file;

		obs_video_info video{};
		obs_get_video_info(&video);

		Header header{
			.version = VERSION,
			.game_timing = (uint32_t)game_timing::status(),
			.qpc_frequency = win::qpc_frequency(),
			.saved_qpc = saved_qpc,
			.fps_num = video.fps_num,
			.fps_den = video.fps_den,
			.render_delay = render_delay(output),
		};
		std::ranges::copy(std::string_view{ "BLURFTIM" }, header.magic);

		write_record(file, header);
		return file;
	}

	bool set_saved_qpc(std::ostream& out, int64_t saved_qpc) {
		std::ostream::pos_type end = out.tellp();
		out.seekp(offsetof(Header, saved_qpc));
		bool ok = write_record(out, saved_qpc);
		out.seekp(end);
		return ok;
	}

	bool write_batch_header(std::ostream& out, std::string_view tag, uint32_t count) {
		BatchHeader header{ .count = count };
		std::ranges::copy(tag, header.tag);
		return write_record(out, header);
	}

	bool write_replay(std::string_view path, int64_t saved_qpc, int64_t from_qpc, obs_output_t* output) {
		// taken before the file is opened so a slow disk doesn't hold the logs up
		auto ticks = since(logs::tick, from_qpc, &TickRecord::qpc);
		auto reads = since(logs::read, from_qpc, &ReadRecord::submitted_qpc);
		auto packets = since(logs::replay_packets.records, from_qpc, &PacketRecord::received_qpc);
		auto presents = since(logs::present, from_qpc, &PresentRecord::present_start);

		// the logs are sized in records, so a game presenting fast enough outruns what a replay needs
		if (!presents.empty() && logs::present.wrapped() && (int64_t)presents.front().present_start > from_qpc)
			obs_log(
				LOG_WARNING,
				"the game's frames are only logged from %.0fs before this replay was saved - it presents faster "
				"than the log holds",
				(double)(saved_qpc - (int64_t)presents.front().present_start) / (double)win::qpc_frequency()
			);

		std::ofstream file = open(path, saved_qpc, output);
		bool ok = file && write_batch(file, "TICK", ticks) && write_batch(file, "READ", reads) &&
		          write_batch(file, "PCKT", packets) && write_batch(file, "PRES", presents) &&
		          write_batch(file, "GAME", captured_game.records());

		file.close();
		return ok && file.good();
	}

} // namespace ft::sidecar
