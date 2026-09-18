#pragma once

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

} // namespace ft::game_timing
