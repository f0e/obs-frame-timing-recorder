#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace ft {

	// When the gpu actually ran the probe's draw, from the gpu's own clock.
	//
	// The probe's other measurement - a worker thread waking on the event Flush1 signals - is the gpu
	// finishing the draw plus however long windows took to signal and schedule that thread, which is a
	// machine's scheduling behaviour rather than anything about the recording. A timestamp query either side
	// of the draw measures the draw itself, and the two together say how much of the difference was the wait.
	//
	// The results are collected several frames later, so reading them never waits on the gpu, the same way the
	// fingerprint stages its readbacks.
	//
	// The ticks are the gpu's, not QPC, and d3d11 has no way to ask for the two clocks side by side. So
	// nothing here converts them: the raw ticks and the frequency go in the log and blur lines the clocks up
	// against `done_qpc`, which is in QPC and is late by a non-negative amount.
	class GpuTimer {
	public:
		struct Sample {
			uint64_t sequence;
			uint64_t begin;     // gpu ticks before the draw
			uint64_t end;       // gpu ticks after it
			uint64_t frequency; // gpu ticks per second, 0 if the gpu said the span was disjoint
		};

		// brackets the draw. `end` must follow a `begin` that returned true, before anything else is drawn
		bool begin(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t sequence);
		void end(ID3D11DeviceContext* context);

		// the oldest slot whose results the gpu has finished with, if there is one
		bool collect(ID3D11DeviceContext* context, Sample& sample);

		void release();

		bool failed() const {
			return broken;
		}

	private:
		struct Slot {
			Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
			Microsoft::WRL::ComPtr<ID3D11Query> begin;
			Microsoft::WRL::ComPtr<ID3D11Query> end;
			uint64_t sequence = 0;
			bool waiting = false;
		};

		bool build(ID3D11Device* device);

		std::vector<Slot> slots;
		size_t next = 0;
		size_t open = SIZE_MAX; // the slot `begin` opened, while it's open
		bool broken = false;
	};

} // namespace ft
