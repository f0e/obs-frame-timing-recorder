#include "sidecar.hpp"
#include "game.hpp"
#include "game_timing.hpp"
#include "logs.hpp"
#include "win.hpp"

#include <obs.h>

#include <algorithm>
#include <cstddef>

namespace ft {

	namespace {

#pragma pack(push, 1)

		struct Header {
			char magic[8];
			uint32_t version;
			uint32_t game_timing; // GameTimingStatus
			int64_t qpc_frequency;
			int64_t saved_qpc;
			uint32_t fps_num;
			uint32_t fps_den;
		};

		struct BatchHeader {
			char tag[4];
			uint32_t count;
		};

#pragma pack(pop)

		// blur's frame_timing.py unpacks these, so they have to stay as they are
		static_assert(sizeof(Header) == 40);
		static_assert(offsetof(Header, saved_qpc) == 24);
		static_assert(sizeof(BatchHeader) == 8);

		constexpr uint32_t SIDECAR_VERSION = 6;

		template<typename T>
		bool write_record(std::ostream& out, const T& record) {
			out.write(reinterpret_cast<const char*>(&record), sizeof(record));
			return (bool)out;
		}

	} // namespace

	std::ofstream open_sidecar(std::string_view path, int64_t saved_qpc) {
		std::ofstream file(as_path(path), std::ios::binary);
		if (!file)
			return file;

		obs_video_info video{};
		obs_get_video_info(&video);

		Header header{
			.version = SIDECAR_VERSION,
			.game_timing = (uint32_t)game_timing_status(),
			.qpc_frequency = qpc_frequency(),
			.saved_qpc = saved_qpc,
			.fps_num = video.fps_num,
			.fps_den = video.fps_den,
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

	bool write_replay_sidecar(std::string_view path, int64_t saved_qpc, int64_t from_qpc) {
		// taken before the file is opened so a slow disk doesn't hold the logs up
		auto ticks = since(tick_log, from_qpc, &TickRecord::qpc);
		auto reads = since(read_log, from_qpc, &ReadRecord::submitted_qpc);
		auto packets = since(replay_packets.records, from_qpc, &PacketRecord::received_qpc);
		auto presents = since(present_log, from_qpc, &PresentRecord::present_start);

		std::ofstream file = open_sidecar(path, saved_qpc);
		bool ok = file && write_batch(file, "TICK", ticks) && write_batch(file, "READ", reads) &&
		          write_batch(file, "PCKT", packets) && write_batch(file, "PRES", presents) &&
		          write_batch(file, "GAME", captured_game.records());

		file.close();
		return ok && file.good();
	}

} // namespace ft
