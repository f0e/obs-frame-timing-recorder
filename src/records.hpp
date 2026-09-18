#pragma once

#include <cstdint>

// records go into the sidecar exactly as they are in memory, so every field is 8 bytes and nothing is padded.
// blur's frame_timing.py reads them back

namespace ft {

	// one per obs video tick, from the tick callback
	struct TickRecord {
		int64_t qpc;
		uint64_t frame_time; // obs_get_video_frame_time, the tick's ideal time in os_gettime_ns
		uint64_t total_frames;
		uint64_t lagged_frames;
	};

	// one per tick that rendered the game capture for the output. the draw is followed by a flush whose
	// completion the gpu reports, which is when the draw - and so the read of the captured picture - really ran
	struct ReadRecord {
		uint64_t frame_time;
		int64_t submitted_qpc;
		int64_t done_qpc; // 0 if the gpu never reported it
	};

	// one per encoded video packet an output received
	struct PacketRecord {
		int64_t pts;
		int64_t dts;
		int64_t dts_usec;
		int64_t sys_dts_usec;
		uint64_t size;
		uint64_t keyframe;
		uint64_t cts; // encoder_packet_time, 0 when obs didn't give one
		uint64_t fer;
		uint64_t ferc;
		int64_t received_qpc;
	};

	enum PresentFlags : uint64_t {
		PRESENT_LOST = 1 << 0,
		PRESENT_FAILED = 1 << 1,
	};

	// one per present from any process but obs, from presentdata. times are qpc
	struct PresentRecord {
		uint64_t present_start;   // when the present call began
		uint64_t time_in_present; // how long it took, in qpc ticks
		uint64_t gpu_start;       // when the frame's first gpu work started, 0 if unknown
		uint64_t ready;           // when the frame's last gpu work finished, 0 if unknown
		uint64_t gpu_duration;    // qpc ticks the frame's gpu work was actually running
		uint64_t swap_chain;
		uint64_t process_id;
		uint64_t runtime;      // presentdata's Runtime
		uint64_t present_mode; // presentdata's PresentMode
		uint64_t final_state;  // presentdata's PresentResult
		uint64_t flags;        // PresentFlags
		// when the game says it started simulating this frame, 0 if it doesn't say. from intel's presentmon
		// markers and from nvidia reflex's
		uint64_t app_sim_start;
		uint64_t reflex_sim_start;
		// when the frame reached the screen, 0 if it never did. that is what a window capture sees, since its
		// frames come from the compositor rather than from the game's own buffer
		uint64_t screen_time;
		uint64_t window; // the hwnd it was presented to, 0 if the trace didn't say
	};

	// how obs was capturing, which decides when a game frame becomes something obs can read
	enum class CaptureKind : uint64_t {
		// game capture: obs's hook copies the game's own buffer as it presents
		HOOK = 0,
		// window capture: the compositor hands obs the window as it goes to the screen
		WINDOW = 1,
		OTHER = 2,
	};

	// a game obs captured while the log was running, and so whose presents it holds
	struct GameRecord {
		uint64_t process_id;
		uint64_t capture; // CaptureKind
		uint64_t window;  // the hwnd obs captured, 0 if it wasn't found
		char name[120];
	};

	static_assert(sizeof(TickRecord) == 32);
	static_assert(sizeof(ReadRecord) == 24);
	static_assert(sizeof(PacketRecord) == 80);
	static_assert(sizeof(PresentRecord) == 120);
	static_assert(sizeof(GameRecord) == 144);

	enum class GameTimingStatus : uint32_t {
		TRACING = 0,
		NO_PERMISSION = 1,
		FAILED = 2,
		NOT_STARTED = 3,
	};

} // namespace ft
