#pragma once

#include <chrono>
#include <cstdint>

namespace ft::game_timing {

	// what the sidecar header records, so the numbers have to stay as they are
	enum class Status : uint32_t {
		TRACING = 0,
		NO_PERMISSION = 1,
		FAILED = 2,
		NOT_STARTED = 3,
	};

	Status start();
	void stop();
	Status status();

	// etw hands its buffered events over on a timer of its own - a second, for a realtime session - so a log
	// closed right after a recording ends is missing that last second of game frames. this asks for them and
	// waits until the game's presents reach `until`, or until it gives up. blocks, so not on obs's threads
	bool drain(int64_t until, std::chrono::milliseconds give_up_after);

} // namespace ft::game_timing
