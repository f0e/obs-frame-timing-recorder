#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ft {

	// a fixed size log that keeps the newest entries. entries are numbered in the order they were pushed, so one
	// can be filled in later as long as it hasn't been overwritten since
	template<typename T>
	class Ring {
	public:
		explicit Ring(size_t capacity) : items(capacity) {}

		uint64_t push(const T& item) {
			std::lock_guard lock(mutex);
			uint64_t sequence = written_++;
			items[sequence % items.size()] = item;
			return sequence;
		}

		void update(uint64_t sequence, std::invocable<T&> auto&& change) {
			std::lock_guard lock(mutex);
			if (sequence < written_ && written_ - sequence <= items.size())
				change(items[sequence % items.size()]);
		}

		uint64_t written() const {
			std::lock_guard lock(mutex);
			return written_;
		}

		// whether anything pushed has been overwritten, so the log no longer reaches back as far as it was asked
		bool wrapped() const {
			std::lock_guard lock(mutex);
			return written_ > items.size();
		}

		std::vector<T> snapshot() const {
			uint64_t from = 0;
			return read_from(from, [](const T&) {
				return true;
			});
		}

		// everything from `sequence` on that `ready` accepts, stopping at the first it doesn't, and moves
		// `sequence` past what was read. entries already overwritten are skipped
		std::vector<T> read_from(uint64_t& sequence, std::predicate<const T&> auto&& ready) const {
			std::lock_guard lock(mutex);
			uint64_t oldest = written_ - std::min(written_, items.size());
			sequence = std::max(sequence, oldest);

			std::vector<T> out;
			out.reserve(written_ - sequence);
			for (; sequence < written_; sequence++) {
				const T& item = items[sequence % items.size()];
				if (!ready(item))
					break;
				out.push_back(item);
			}
			return out;
		}

	private:
		mutable std::mutex mutex;
		std::vector<T> items;
		uint64_t written_ = 0;
	};

	inline constexpr auto anything = [](const auto&) {
		return true;
	};

	// the log's entries from `from` on, timed by `time_of` - a member pointer or a function
	template<typename T>
	std::vector<T> since(const Ring<T>& log, int64_t from, auto&& time_of) {
		std::vector<T> out = log.snapshot();
		std::erase_if(out, [&](const T& item) {
			return (int64_t)std::invoke(time_of, item) < from;
		});
		return out;
	}

} // namespace ft
