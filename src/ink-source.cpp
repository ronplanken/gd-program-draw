// SPDX-License-Identifier: GPL-2.0-or-later
#include "ink-source.hpp"

namespace program_draw {
static std::shared_ptr<Canvas> sharedCanvas;
struct InkSource {
	std::shared_ptr<Canvas> canvas;
	gs_texture_t *texture = nullptr;
	uint64_t uploadedRevision = 0;
};
static const char *sourceName(void *)
{
	return "Program Draw POC (internal overlay)";
}
static void *create(obs_data_t *, obs_source_t *)
{
	return new InkSource{sharedCanvas};
}
static void destroy(void *data)
{
	auto *source = static_cast<InkSource *>(data);
	obs_enter_graphics();
	gs_texture_destroy(source->texture);
	obs_leave_graphics();
	delete source;
}
static uint32_t width(void *data)
{
	return static_cast<InkSource *>(data)->canvas->size().width();
}
static uint32_t height(void *data)
{
	return static_cast<InkSource *>(data)->canvas->size().height();
}
static void render(void *data, gs_effect_t *)
{
	auto &source = *static_cast<InkSource *>(data);
	auto snapshot = source.canvas->snapshot(source.uploadedRevision);
	if (snapshot.empty)
		return;
	if (!snapshot.image.isNull()) {
		// Use straight RGBA for correct antialiased transparency with OBS's blend state.
		QImage rgba = snapshot.image.convertToFormat(QImage::Format_RGBA8888);
		if (source.texture && (gs_texture_get_width(source.texture) != uint32_t(rgba.width()) ||
				       gs_texture_get_height(source.texture) != uint32_t(rgba.height()))) {
			gs_texture_destroy(source.texture);
			source.texture = nullptr;
		}
		if (!source.texture)
			source.texture =
				gs_texture_create(rgba.width(), rgba.height(), GS_RGBA, 1, nullptr, GS_DYNAMIC);
		if (!source.texture)
			return;
		gs_texture_set_image(source.texture, rgba.constBits(), uint32_t(rgba.bytesPerLine()), false);
		source.uploadedRevision = snapshot.revision;
	}
	if (!source.texture)
		return;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	const bool linearSrgb = gs_get_linear_srgb();
	gs_enable_framebuffer_srgb(linearSrgb);
	auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	auto *image = gs_effect_get_param_by_name(effect, "image");
	if (linearSrgb)
		gs_effect_set_texture_srgb(image, source.texture);
	else
		gs_effect_set_texture(image, source.texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(source.texture, 0, 0, 0);
	gs_enable_framebuffer_srgb(previousSrgb);
	gs_blend_state_pop();
}
void registerInkSource(std::shared_ptr<Canvas> canvas)
{
	sharedCanvas = std::move(canvas);
	obs_source_info info{};
	info.id = SourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	info.get_name = sourceName;
	info.create = create;
	info.destroy = destroy;
	info.get_width = width;
	info.get_height = height;
	info.video_render = render;
	obs_register_source(&info);
}
} // namespace program_draw
