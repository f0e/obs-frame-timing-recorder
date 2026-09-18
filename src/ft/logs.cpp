#include "logs.hpp"
#include "win.hpp"

namespace ft::logs {

	void PacketLog::on_packet(obs_output_t*, encoder_packet* packet, encoder_packet_time* time, void* param) {
		if (packet->type != OBS_ENCODER_VIDEO)
			return;

		static_cast<PacketLog*>(param)->records.push(
			{
				packet->pts,
				packet->dts,
				packet->dts_usec,
				packet->sys_dts_usec,
				packet->size,
				packet->keyframe ? 1u : 0u,
				time ? time->cts : 0,
				time ? time->fer : 0,
				time ? time->ferc : 0,
				win::qpc_now(),
			}
		);
	}

	void PacketLog::attach(obs_output_t* next) {
		if (output == next)
			return;

		detach();
		output = next; // takes a reference of its own
		if (output)
			obs_output_add_packet_callback(output, on_packet, this);
	}

	void PacketLog::detach() {
		if (!output)
			return;

		obs_output_remove_packet_callback(output, on_packet, this);
		output = nullptr;
	}

} // namespace ft::logs
