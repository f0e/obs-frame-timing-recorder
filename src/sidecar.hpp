#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace ft {

constexpr std::string_view SIDECAR_SUFFIX = ".frametiming";

template<typename T> bool write_records(std::ostream &out, std::span<const T> records)
{
	out.write(reinterpret_cast<const char *>(records.data()), (std::streamsize)records.size_bytes());
	return (bool)out;
}

template<typename T> bool write_record(std::ostream &out, const T &record)
{
	return write_records(out, std::span<const T>{&record, 1});
}

// one section of a sidecar: its tag, its record size and count, and how to write its records. sections are
// written straight after they're made, so what `write` refers to has to outlive it
struct Section {
	std::string_view tag;
	uint32_t record_size;
	uint64_t count;
	std::function<bool(std::ostream &)> write;
};

template<typename T> Section section_of(std::string_view tag, const std::vector<T> &records)
{
	return {tag, sizeof(T), records.size(),
		[&records](std::ostream &out) { return write_records(out, std::span{records}); }};
}

// the sidecar blur reads: TICK, READ, PCKT, PRES and PROC, in that order
bool write_sidecar(std::string_view path, int64_t saved_qpc, std::span<const Section> sections);

// a replay's sidecar, from what was logged from `from_qpc` on
bool write_replay_sidecar(std::string_view path, int64_t saved_qpc, int64_t from_qpc);

} // namespace ft
