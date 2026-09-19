#pragma once

#include <obs.h>

#include <cstdint>
#include <vector>

namespace ft {

	// A fingerprint of the picture obs read, so that "is this recorded frame a new picture" is measured rather
	// than worked out from the pictures afterwards.
	//
	// The filter renders the capture into a texture of its own and draws the output from that, so the picture
	// this hashes is exactly the one that was encoded - not a second read of the shared texture, which the
	// hook could have written to in between.
	//
	// The reduction is a chain of halvings, each one a bilinear draw whose samples land on four texel centres,
	// so it's an exact box filter down to a small image every pixel has a hand in. It stops at CELLS, which is
	// small enough to read back and hash for nothing and large enough that a cell covers a few hundred pixels:
	// anything that moves changes a cell by more than a byte.
	class Fingerprint {
	public:
		// renders the filter's target into a texture and hands it back to be drawn, nothing on failure
		gs_texture_t* capture(obs_source_t* filter, uint32_t cx, uint32_t cy, gs_color_space space);

		// reduces the captured picture and stages it. reads back the oldest staged picture, which by now the
		// gpu has long finished, and hands its hash to `store`
		template<typename Store>
		void hash_older(Store&& store) {
			uint64_t sequence = 0;
			uint64_t hash = 0;
			if (take_older(sequence, hash))
				store(sequence, hash);
		}

		void reduce(uint64_t sequence);

		// the graphics context has to be current
		void release();

		bool failed() const {
			return broken;
		}

	private:
		struct Staged {
			gs_stagesurf_t* surface = nullptr;
			uint64_t sequence = 0;
			bool waiting = false;
		};

		bool take_older(uint64_t& sequence, uint64_t& hash);
		void rebuild(uint32_t cx, uint32_t cy, gs_color_format format);

		gs_texrender_t* picture = nullptr;
		// the halvings, ending at the one that gets staged
		std::vector<gs_texrender_t*> steps;
		std::vector<Staged> staged;
		size_t next_stage = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		gs_color_format picture_format = GS_RGBA;
		gs_color_space picture_space = GS_CS_SRGB;
		bool broken = false;
	};

} // namespace ft
