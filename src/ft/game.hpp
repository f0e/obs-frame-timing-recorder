#pragma once

#include "records.hpp"

#include <obs.h>
#include <obs.hpp>

#include <mutex>
#include <vector>

namespace ft {

	// the process obs is capturing, from the game capture or window capture source the probe filter sits on.
	// nothing else can be in the recording, so its presents are the only ones worth logging - which keeps the
	// logs small and saves blur guessing which process was the game
	class CapturedGame {
	public:
		// follows whichever source the probe filter sits on, for as long as it sits there
		void watch(obs_source_t* capture);
		void unwatch();

		bool presented(uint64_t process_id) const;
		std::vector<GameRecord> records() const;

	private:
		void hooked(const char* executable, const char* title, const char* window_class);

		mutable std::mutex mutex;
		OBSSignal on_hooked;
		CaptureKind kind = CaptureKind::OTHER;
		// every game captured while the plugin has been running: one that restarts comes back under a new id
		std::vector<GameRecord> games;
	};

	inline CapturedGame captured_game;

} // namespace ft
