#include "fingerprint.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>

namespace ft {

	namespace {

		// the reduction stops once both sides are this small. at 1080p that leaves 60x34 cells of about 570
		// pixels each, so a byte of change anywhere in a cell shows up in it
		constexpr uint32_t CELLS = 64;

		// how many pictures are staged before one is read back. the read has to be of a copy the gpu finished
		// long ago, because mapping one it hasn't waits for it - and obs's gpu work runs several frames behind
		// when something else owns the gpu
		constexpr size_t STAGES = 16;

		uint32_t bytes_per_pixel(gs_color_format format) {
			switch (format) {
				case GS_RGBA16F:
				case GS_RGBA16:
					return 8;
				case GS_RGBA32F:
					return 16;
				default:
					return 4;
			}
		}

		uint64_t fnv1a(const uint8_t* bytes, size_t count, uint64_t hash) {
			for (size_t i = 0; i < count; i++) {
				hash ^= bytes[i];
				hash *= 0x00000100000001b3;
			}
			return hash;
		}

		void draw_into(gs_texrender_t* target, gs_texture_t* source, uint32_t cx, uint32_t cy) {
			gs_texrender_reset(target);
			if (!gs_texrender_begin(target, cx, cy))
				return;

			gs_effect_t* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), source);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(source, 0, cx, cy);

			gs_texrender_end(target);
		}

	} // namespace

	gs_texture_t* Fingerprint::capture(obs_source_t* filter, uint32_t cx, uint32_t cy, gs_color_space space) {
		obs_source_t* target = obs_filter_get_target(filter);
		obs_source_t* parent = obs_filter_get_parent(filter);
		if (broken || !target || !parent || !cx || !cy)
			return nullptr;

		gs_color_format format = gs_get_format_from_space(space);
		if (cx != width || cy != height || format != picture_format)
			rebuild(cx, cy, format);
		if (broken)
			return nullptr;

		picture_space = space;

		// the same sequence obs uses to render a filter's input, so the picture is what it would have been
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_texrender_reset(picture);
		if (gs_texrender_begin_with_color_space(picture, cx, cy, space)) {
			uint32_t flags = obs_source_get_output_flags(target);
			struct vec4 clear{};

			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			bool plain = !(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC));
			if (target == parent && plain)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);

			gs_texrender_end(picture);
		}

		gs_blend_state_pop();

		return gs_texrender_get_texture(picture);
	}

	void Fingerprint::reduce(uint64_t sequence) {
		if (broken || steps.empty())
			return;

		gs_texture_t* source = gs_texrender_get_texture(picture);
		if (!source)
			return;

		// raw values throughout: the hash only has to say whether the picture changed, so nothing is gained by
		// converting anything and a conversion is one more thing to be sure of
		bool srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);

		uint32_t cx = width;
		uint32_t cy = height;
		for (gs_texrender_t* step : steps) {
			cx = std::max(cx / 2, 1u);
			cy = std::max(cy / 2, 1u);
			draw_into(step, source, cx, cy);
			source = gs_texrender_get_texture(step);
			if (!source)
				break;
		}

		gs_enable_framebuffer_srgb(srgb);

		Staged& slot = staged[next_stage];
		if (source && slot.surface) {
			gs_stage_texture(slot.surface, source);
			slot.sequence = sequence;
			slot.waiting = true;
		}
		next_stage = (next_stage + 1) % staged.size();
	}

	bool Fingerprint::take_older(uint64_t& sequence, uint64_t& hash) {
		if (broken || staged.empty())
			return false;

		// the slot the next picture goes into is the one staged longest ago
		Staged& slot = staged[next_stage];
		if (!slot.waiting || !slot.surface)
			return false;

		slot.waiting = false;

		uint8_t* bytes = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(slot.surface, &bytes, &linesize))
			return false;

		uint32_t rows = gs_stagesurface_get_height(slot.surface);
		uint32_t row =
			gs_stagesurface_get_width(slot.surface) * bytes_per_pixel(gs_stagesurface_get_color_format(slot.surface));
		// the padding at the end of a row is whatever was in the memory, so only the picture is hashed
		uint64_t value = 0xcbf29ce484222325;
		for (uint32_t y = 0; y < rows; y++)
			value = fnv1a(bytes + (size_t)y * linesize, std::min(row, linesize), value);

		gs_stagesurface_unmap(slot.surface);

		sequence = slot.sequence;
		hash = value ? value : 1; // 0 means "not worked out"
		return true;
	}

	void Fingerprint::rebuild(uint32_t cx, uint32_t cy, gs_color_format format) {
		release();

		width = cx;
		height = cy;
		picture_format = format;

		picture = gs_texrender_create(format, GS_ZS_NONE);

		uint32_t step_cx = cx;
		uint32_t step_cy = cy;
		while (step_cx > CELLS || step_cy > CELLS) {
			step_cx = std::max(step_cx / 2, 1u);
			step_cy = std::max(step_cy / 2, 1u);
			steps.push_back(gs_texrender_create(format, GS_ZS_NONE));
		}

		staged.resize(STAGES);
		for (Staged& slot : staged)
			slot.surface = gs_stagesurface_create(step_cx, step_cy, format);

		broken = !picture || std::ranges::any_of(steps, [](gs_texrender_t* step) {
			return !step;
		}) || std::ranges::any_of(staged, [](const Staged& slot) {
			return !slot.surface;
		});

		if (broken) {
			obs_log(LOG_WARNING, "couldn't set up picture fingerprints (%ux%u, format %d)", cx, cy, format);
			release();
			broken = true;
		}
		else {
			obs_log(
				LOG_INFO,
				"fingerprinting the captured picture: %ux%u reduced to %ux%u in %zu steps",
				cx,
				cy,
				step_cx,
				step_cy,
				steps.size()
			);
		}
	}

	void Fingerprint::release() {
		for (Staged& slot : staged) {
			if (slot.surface)
				gs_stagesurface_destroy(slot.surface);
		}
		staged.clear();
		next_stage = 0;

		for (gs_texrender_t* step : steps)
			gs_texrender_destroy(step);
		steps.clear();

		if (picture)
			gs_texrender_destroy(picture);
		picture = nullptr;

		width = height = 0;
		broken = false;
	}

} // namespace ft
