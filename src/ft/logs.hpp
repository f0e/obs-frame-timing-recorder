#pragma once

#include "records.hpp"
#include "ring.hpp"

#include <obs.h>
#include <obs.hpp>

namespace ft::logs {

	// about 12 minutes of each at 360fps - more than a replay buffer usually holds, and far more than a recording
	// needs between two looks
	inline Ring<TickRecord> tick{ 1 << 18 };
	inline Ring<ReadRecord> read{ 1 << 18 };
	// a game at 1000fps for about 4 minutes, plus whatever else presents
	inline Ring<PresentRecord> present{ 1 << 18 };

	// the encoded video packets one output receives. the replay buffer and the recording each have their own,
	// since both can run at once and matching a file to its packets needs one output's log
	class PacketLog {
	public:
		void attach(obs_output_t* output);
		void detach();

		Ring<PacketRecord> records{ 1 << 18 };

	private:
		static void on_packet(obs_output_t*, encoder_packet* packet, encoder_packet_time* time, void* param);

		OBSOutput output;
	};

	inline PacketLog replay_packets;
	inline PacketLog recording_packets;

} // namespace ft::logs
