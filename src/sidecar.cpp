#include "sidecar.hpp"
#include "game_timing.hpp"
#include "logs.hpp"
#include "process_names.hpp"
#include "win.hpp"

#include <obs.h>

#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace ft {

namespace {

#pragma pack(push, 1)
struct Header {
	char magic[8];
	uint32_t version;
	uint32_t sections;
	int64_t qpc_frequency;
	int64_t saved_qpc;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t game_timing; // GameTimingStatus
	uint32_t reserved;
};

struct SectionHeader {
	char tag[4];
	uint32_t record_size;
	uint64_t count;
};
#pragma pack(pop)

constexpr uint32_t SIDECAR_VERSION = 3;

ProcessNames replay_process_names;

} // namespace

bool write_sidecar(std::string_view path, int64_t saved_qpc, std::span<const Section> sections)
{
	std::ofstream file(as_path(path), std::ios::binary);
	if (!file)
		return false;

	obs_video_info video{};
	obs_get_video_info(&video);

	Header header{
		.version = SIDECAR_VERSION,
		.sections = (uint32_t)sections.size(),
		.qpc_frequency = qpc_frequency(),
		.saved_qpc = saved_qpc,
		.fps_num = video.fps_num,
		.fps_den = video.fps_den,
		.game_timing = (uint32_t)game_timing_status(),
	};
	std::ranges::copy(std::string_view{"BLURFTIM"}, header.magic);

	if (!write_record(file, header))
		return false;

	for (const Section &section : sections) {
		SectionHeader section_header{.record_size = section.record_size, .count = section.count};
		std::ranges::copy(section.tag, section_header.tag);
		if (!write_record(file, section_header) || !section.write(file))
			return false;
	}

	file.close();
	return file.good();
}

bool write_replay_sidecar(std::string_view path, int64_t saved_qpc, int64_t from_qpc)
{
	// taken before the file is opened so a slow disk doesn't hold the logs up
	auto ticks = since(tick_log, from_qpc, &TickRecord::qpc);
	auto reads = since(read_log, from_qpc, &ReadRecord::submitted_before_qpc);
	auto packets = since(replay_packets.records, from_qpc, &PacketRecord::received_qpc);
	auto presents = since(present_log, from_qpc, &PresentRecord::present_start);

	std::unordered_set<uint64_t> seen;
	for (const PresentRecord &present : presents)
		seen.insert(present.process_id);

	std::vector<uint64_t> process_ids(seen.begin(), seen.end());
	for (uint64_t process_id : process_ids)
		replay_process_names.learn(process_id);

	std::erase_if(presents, [](const PresentRecord &present) {
		return never_the_game(replay_process_names.name(present.process_id));
	});
	auto processes = replay_process_names.records(process_ids);

	const Section sections[] = {
		section_of("TICK", ticks),    section_of("READ", reads),     section_of("PCKT", packets),
		section_of("PRES", presents), section_of("PROC", processes),
	};
	return write_sidecar(path, saved_qpc, sections);
}

} // namespace ft
