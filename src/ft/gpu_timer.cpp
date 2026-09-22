#include "gpu_timer.hpp"

#include <obs-module.h>
#include <plugin-support.h>

namespace ft {

	namespace {

		// how many frames a slot's results are left for before they're read. the fingerprint uses the same
		// number for the same reason: by then the gpu is long done, so nothing ever waits on it
		constexpr size_t SLOTS = 16;

	} // namespace

	bool GpuTimer::build(ID3D11Device* device) {
		slots.resize(SLOTS);
		for (Slot& slot : slots) {
			D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			D3D11_QUERY_DESC stamp{ D3D11_QUERY_TIMESTAMP, 0 };
			if (FAILED(device->CreateQuery(&disjoint, &slot.disjoint)) ||
			    FAILED(device->CreateQuery(&stamp, &slot.begin)) || FAILED(device->CreateQuery(&stamp, &slot.end)))
			{
				obs_log(LOG_WARNING, "the gpu wouldn't give timestamp queries - falling back to the flush event");
				broken = true;
				slots.clear();
				return false;
			}
		}
		return true;
	}

	bool GpuTimer::begin(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t sequence) {
		if (broken)
			return false;
		if (slots.empty() && !build(device))
			return false;

		Slot& slot = slots[next];
		// still holding results nobody collected, so this frame goes unmeasured rather than losing them
		if (slot.waiting)
			return false;

		context->Begin(slot.disjoint.Get());
		context->End(slot.begin.Get());

		slot.sequence = sequence;
		open = next;
		return true;
	}

	void GpuTimer::end(ID3D11DeviceContext* context) {
		if (open >= slots.size())
			return;

		Slot& slot = slots[open];
		context->End(slot.end.Get());
		context->End(slot.disjoint.Get());

		slot.waiting = true;
		open = SIZE_MAX;
		next = (next + 1) % slots.size();
	}

	bool GpuTimer::collect(ID3D11DeviceContext* context, Sample& sample) {
		if (broken || slots.empty())
			return false;

		// the slot the next draw will take is the one that has been waiting longest
		Slot& slot = slots[next];
		if (!slot.waiting)
			return false;

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT span{};
		UINT64 began = 0;
		UINT64 ended = 0;

		// DONOTFLUSH because this must never push work along just to answer; a slot that isn't ready yet is
		// simply left for next frame
		const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
		if (context->GetData(slot.disjoint.Get(), &span, sizeof(span), flags) != S_OK ||
		    context->GetData(slot.begin.Get(), &began, sizeof(began), flags) != S_OK ||
		    context->GetData(slot.end.Get(), &ended, sizeof(ended), flags) != S_OK)
			return false;

		slot.waiting = false;

		// a disjoint span's ticks can't be compared with any other span's, so it's reported as unusable
		// rather than quietly wrong
		sample = { slot.sequence, began, ended, span.Disjoint ? 0 : span.Frequency };
		return true;
	}

	void GpuTimer::release() {
		slots.clear();
		next = 0;
		open = SIZE_MAX;
		broken = false;
	}

} // namespace ft
