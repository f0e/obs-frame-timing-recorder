#pragma once

#include <cstdint>
#include <fstream>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace ft {

constexpr std::string_view SIDECAR_SUFFIX = ".frametiming";

// a sidecar is a header followed by batches of records, each tagged with what it holds: TICK, READ, PCKT,
// PRES or GAME. a replay writes one batch per tag, a recording appends more as it runs, so nothing has to be
// held anywhere until it stops. blur's frame_timing.py reads them back
std::ofstream open_sidecar(std::string_view path, int64_t saved_qpc);

// everything in a sidecar is relative to this, which a recording only knows once it ends
bool set_saved_qpc(std::ostream &out, int64_t saved_qpc);

bool write_batch_header(std::ostream &out, std::string_view tag, uint32_t count);

template<typename T> bool write_batch(std::ostream &out, std::string_view tag, std::span<const T> records)
{
	if (records.empty())
		return true;
	if (!write_batch_header(out, tag, (uint32_t)records.size()))
		return false;

	out.write(reinterpret_cast<const char *>(records.data()), (std::streamsize)records.size_bytes());
	return (bool)out;
}

template<typename T> bool write_batch(std::ostream &out, std::string_view tag, const std::vector<T> &records)
{
	return write_batch(out, tag, std::span{records});
}

// a replay's sidecar, from what was logged from `from_qpc` on
bool write_replay_sidecar(std::string_view path, int64_t saved_qpc, int64_t from_qpc);

} // namespace ft
