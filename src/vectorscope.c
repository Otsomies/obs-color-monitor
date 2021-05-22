#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-frontend-api.h>
#include "plugin-macros.generated.h"
#include "obs-convenience.h"
#include "common.h"

#define debug(format, ...)

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "vss_render";
static const char *prof_render_name_b = "vss_render_bypass";
static const char *prof_render_target_name = "render_target";
static const char *prof_convert_yuv_name = "convert_yuv";
static const char *prof_stage_surface_name = "stage_surface";
static const char *prof_draw_vectorscope_name = "draw_vectorscope";
static const char *prof_draw_name = "draw";
static const char *prof_draw_graticule_name = "graticule";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define VS_SIZE 256
#define SOURCE_CHECK_NS 3000000000
#define N_GRATICULES 18
#define SKIN_TONE_LINE 0x99ABCB // BGR

gs_effect_t *vss_effect = NULL;

#define RGB2Y_601(r, g, b) ((+306*(r) +601*(g) +117*(b))/1024 +  0)
#define RGB2U_601(r, g, b) ((-150*(r) -296*(g) +448*(b))/1024 +128)
#define RGB2V_601(r, g, b) ((+448*(r) -374*(g) - 72*(b))/1024 +128)

#define RGB2Y_709(r, g, b) ((+218*(r) +732*(g) + 74*(b))/1024 + 16)
#define RGB2U_709(r, g, b) ((-102*(r) -346*(g) +450*(b))/1024 +128)
#define RGB2V_709(r, g, b) ((+450*(r) -408*(g) - 40*(b))/1024 +128)

struct vss_source
{
	obs_source_t *self;
	gs_texrender_t *texrender;
	gs_texrender_t *texrender_uv;
	gs_stagesurf_t* stagesurface;
	uint32_t known_width;
	uint32_t known_height;

	gs_texture_t *tex_vs;
	uint8_t *tex_buf;

	pthread_mutex_t target_update_mutex;
	uint64_t target_check_time;
	obs_weak_source_t *weak_target;
	char *target_name;

	gs_image_file_t graticule_img;
	gs_vertbuffer_t *graticule_vbuf;
	gs_vertbuffer_t *graticule_line_vbuf;

	int target_scale;
	int intensity;
	int graticule;
	int graticule_skintone_color;
	int colorspace;
	int colorspace_calc; // get from ovi if auto
	bool update_graticule;
	bool bypass_vectorscope;

	bool rendered;
	bool enumerating; // not thread safe but I have no other idea.
};

static const char *vss_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Vectorscope");
}

static void vss_update(void *, obs_data_t *);

static void *vss_create(obs_data_t *settings, obs_source_t *source)
{
	struct vss_source *src = bzalloc(sizeof(struct vss_source));

	src->self = source;
	obs_enter_graphics();
	src->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	src->texrender_uv = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!vss_effect) {
		char *f = obs_module_file("vectorscope.effect");
		vss_effect = gs_effect_create_from_file(f, NULL);
		if (!vss_effect)
			blog(LOG_ERROR, "Cannot load '%s'", f);
		bfree(f);
	}
	obs_leave_graphics();

	{
		// The file is generated by
		// inkscape --export-png=data/vectorscope-graticule.png --export-area-page src/vectorscope-graticule.svg
		char *f = obs_module_file("vectorscope-graticule.png");
		gs_image_file_init(&src->graticule_img, f);
		if (!src->graticule_img.loaded)
			blog(LOG_ERROR, "Cannot load '%s'", f);
		obs_enter_graphics();
		gs_image_file_init_texture(&src->graticule_img);
		obs_leave_graphics();
		bfree(f);
	}

	pthread_mutex_init(&src->target_update_mutex, NULL);

	vss_update(src, settings);

	return src;
}

static void vss_destroy(void *data)
{
	struct vss_source *src = data;

	obs_enter_graphics();
	gs_stagesurface_destroy(src->stagesurface);
	gs_texrender_destroy(src->texrender);
	gs_texrender_destroy(src->texrender_uv);

	gs_texture_destroy(src->tex_vs);
	bfree(src->tex_buf);
	gs_image_file_free(&src->graticule_img);
	gs_vertexbuffer_destroy(src->graticule_vbuf);
	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	obs_leave_graphics();

	bfree(src->target_name);
	pthread_mutex_destroy(&src->target_update_mutex);
	obs_weak_source_release(src->weak_target);

	bfree(src);
}

static void vss_update(void *data, obs_data_t *settings)
{
	struct vss_source *src = data;
	obs_weak_source_t *weak_source_old = NULL;

	const char *target_name = obs_data_get_string(settings, "target_name");
	if (!src->target_name || strcmp(target_name, src->target_name)) {
		pthread_mutex_lock(&src->target_update_mutex);
		bfree(src->target_name);
		src->target_name = bstrdup(target_name);
		weak_source_old = src->weak_target;
		src->weak_target = NULL;
		src->target_check_time = os_gettime_ns() - SOURCE_CHECK_NS;
		pthread_mutex_unlock(&src->target_update_mutex);
	}

	if (weak_source_old) {
		obs_weak_source_release(weak_source_old);
		weak_source_old = NULL;
	}

	src->target_scale = (int)obs_data_get_int(settings, "target_scale");
	if (src->target_scale<1)
		src->target_scale = 1;

	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity<1)
		src->intensity = 1;

	src->graticule = (int)obs_data_get_int(settings, "graticule");

	int graticule_skintone_color = (int)obs_data_get_int(settings, "graticule_skintone_color") & 0xFFFFFF;
	if (graticule_skintone_color!=src->graticule_skintone_color) {
		src->graticule_skintone_color = graticule_skintone_color;
		src->update_graticule = 1;
	}

	int colorspace = (int)obs_data_get_int(settings, "colorspace");
	if (colorspace!=src->colorspace) {
		src->colorspace = colorspace;
		src->update_graticule = 1;
	}
	src->bypass_vectorscope = obs_data_get_bool(settings, "bypass_vectorscope");
}

static void vss_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "graticule", 1);
	obs_data_set_default_int(settings, "graticule_skintone_color", SKIN_TONE_LINE);
}

static obs_properties_t *vss_get_properties(void *data)
{
	struct vss_source *src = data;
	obs_properties_t *props;
	obs_property_t *prop;
	props = obs_properties_create();

	prop = obs_properties_add_list(props, "target_name", obs_module_text("Source"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(prop, src ? src->self : NULL);

	obs_properties_add_int(props, "target_scale", obs_module_text("Scale"), 1, 128, 1);
	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	prop = obs_properties_add_list(props, "graticule", obs_module_text("Graticule"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "None", 0);
	obs_property_list_add_int(prop, "Green", 1);

	obs_properties_add_color(props, "graticule_skintone_color", obs_module_text("Skin tone color"));

	prop = obs_properties_add_list(props, "colorspace", obs_module_text("Color space"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Auto", 0);
	obs_property_list_add_int(prop, "601", 1);
	obs_property_list_add_int(prop, "709", 2);

	obs_properties_add_bool(props, "bypass_vectorscope", obs_module_text("Bypass"));

	return props;
}

static uint32_t vss_get_width(void *data)
{
	struct vss_source *src = data;
	return src->bypass_vectorscope ? src->known_width : VS_SIZE;
}

static uint32_t vss_get_height(void *data)
{
	struct vss_source *src = data;
	return src->bypass_vectorscope ? src->known_height : VS_SIZE;
}

static void vss_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct vss_source *src = data;
	if (src->enumerating)
		return;
	src->enumerating = 1;
	obs_source_t *target = obs_weak_source_get_source(src->weak_target);
	if (target) {
		enum_callback(src->self, target, param);
		obs_source_release(target);
	}
	src->enumerating = 0;
}

static inline void vss_draw_vectorscope(struct vss_source *src, uint8_t *video_data, uint32_t video_line)
{
	if (!src->tex_buf)
		src->tex_buf = bzalloc(VS_SIZE*VS_SIZE);
	uint8_t *dbuf = src->tex_buf;

	for (int i=0; i<VS_SIZE*VS_SIZE; i++)
		dbuf[i] = 0;

	const uint32_t height = src->known_height;
	const uint32_t width = src->known_width;
	uint8_t *vd = video_data;
	uint32_t vd_add = video_line - width*4;
	for (uint32_t y=0; y<height; y++) {
		// uint8_t *vd = video_data + video_line * y;
		for (uint32_t x=0; x<width; x++) {
			const uint8_t u = *vd++;
			const uint8_t v = *vd++;
			const uint8_t b = *vd++;
			const uint8_t a = *vd++;
			uint8_t *c = dbuf + (u + VS_SIZE*(255-v));
			if (*c<255) ++*c;
		}
		vd += vd_add;
	}

	if (!src->tex_vs)
		src->tex_vs = gs_texture_create(VS_SIZE, VS_SIZE, GS_R8, 1, (const uint8_t**)&src->tex_buf, GS_DYNAMIC);
	else
		gs_texture_set_image(src->tex_vs, src->tex_buf, VS_SIZE, false);
}

static void vss_render_target(struct vss_source *src)
{
	if (src->rendered)
		return;
	src->rendered = 1;

	obs_source_t *target = src->weak_target ? obs_weak_source_get_source(src->weak_target) : NULL;
	if (!target && *src->target_name)
		return;

	int target_width, target_height;
	if (target) {
		target_width = obs_source_get_width(target);
		target_height = obs_source_get_height(target);
	}
	else {
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		target_width = ovi.base_width;
		target_height = ovi.base_height;
	}
	int width = target_width / src->target_scale;
	int height = target_height / src->target_scale;
	if (width<=0 || height<=0)
		goto end;

	PROFILE_START(prof_render_target_name);

	gs_texrender_reset(src->texrender);
	if (gs_texrender_begin(src->texrender, width, height)) {
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)target_width, 0.0f, (float)target_height, -100.0f, 100.0f);

		gs_blend_state_push();
		if (target) {
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			obs_source_video_render(target);
		}
		else
			obs_render_main_texture();

		gs_texrender_end(src->texrender);

		if (width != src->known_width || height != src->known_height) {
			gs_stagesurface_destroy(src->stagesurface);
			src->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
			src->known_width = width;
			src->known_height = height;
		}

		PROFILE_END(prof_render_target_name);

		if (src->bypass_vectorscope) {
			gs_blend_state_pop();

			goto end;
		}

		gs_texrender_reset(src->texrender_uv);
		if (vss_effect && gs_texrender_begin(src->texrender_uv, width, height)) {
			PROFILE_START(prof_convert_yuv_name);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

			gs_effect_t *effect = vss_effect;
			gs_texture_t *tex = gs_texrender_get_texture(src->texrender);
			if (tex) {
				gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
				while (gs_effect_loop(effect, src->colorspace_calc==1 ? "ConvertRGB_UV601" : "ConvertRGB_UV709")) {
					gs_draw_sprite(tex, 0, width, height);
				}
			}
			gs_texrender_end(src->texrender_uv);
			PROFILE_END(prof_convert_yuv_name);

			PROFILE_START(prof_stage_surface_name);
			gs_stage_texture(src->stagesurface, gs_texrender_get_texture(src->texrender_uv));
			PROFILE_END(prof_stage_surface_name);
			uint8_t *video_data = NULL;
			uint32_t video_linesize;
			PROFILE_START(prof_draw_vectorscope_name);
			if (gs_stagesurface_map(src->stagesurface, &video_data, &video_linesize)) {
				vss_draw_vectorscope(src, video_data, video_linesize);
				gs_stagesurface_unmap(src->stagesurface);
			}
			PROFILE_END(prof_draw_vectorscope_name);
		}
		gs_blend_state_pop();
	}
	else
		PROFILE_END(prof_render_target_name);

end:
	if (target)
		obs_source_release(target);
}

static void create_graticule_vbuf(struct vss_source *src)
{
	if (src->graticule_vbuf)
		return;

	obs_enter_graphics();
	src->graticule_vbuf = create_uv_vbuffer(N_GRATICULES*6, false);
	struct gs_vb_data *vdata = gs_vertexbuffer_get_data(src->graticule_vbuf);
	struct vec2 *tvarray = (struct vec2 *)vdata->tvarray[0].array;
	// copied from FFmpeg vectorscope filter
	const float pp[2][12][2] = {
		{ // 601
			{  90, 240 }, { 240, 110 }, { 166,  16 },
			{  16, 146 }, {  54,  34 }, { 202, 222 },
			{  44, 142 }, { 156,  44 }, {  72,  58 },
			{ 184, 198 }, { 100, 212 }, { 212, 114 },
		},
		{ // 709
			{ 102, 240 }, { 240, 118 }, { 154,  16 },
			{  16, 138 }, {  42,  26 }, { 214, 230 },
			{ 212, 120 }, { 109, 212 }, { 193, 204 },
			{  63,  52 }, { 147,  44 }, {  44, 136 },
		},
	};
	const int ppi = src->colorspace_calc-1;

	// label
	for (int i=0; i<6; i++) {
		float x = pp[ppi][i][0];
		float y = 256.f-pp[ppi][i][1];
		if      (x <  72) y += 20;
		else if (x > 184) y -= 20;
		else if (y > 128) x += 20;
		else              x -= 20;
		set_v3_rect(vdata->points + i*6, x-8, y-8, 16, 16);
		set_v2_uv(tvarray + i*6, i/6.f, 0.f, (i+1)/6.f, 1.f);
	}

	// box
	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	src->graticule_line_vbuf = NULL;
	gs_render_start(true);
	for (int i=0; i<12; i++) {
		const float x = pp[ppi][i][0];
		const float y = 256.f-pp[ppi][i][1];
		const float box[16][2] = {
			{ -6, -6 }, { -2, -6 },
			{ -6, -6 }, { -6, -2 },
			{ +6, -6 }, { +2, -6 },
			{ +6, -6 }, { +6, -2 },
			{ -6, +6 }, { -2, +6 },
			{ -6, +6 }, { -6, +2 },
			{ +6, +6 }, { +2, +6 },
			{ +6, +6 }, { +6, +2 },
		};
		for (int j=0; j<16; j++)
			gs_vertex2f(x+box[j][0], y+box[j][1]);
	}

	// skin tone line
	float stl_u, stl_v, stl_norm;
	int stl_b = src->graticule_skintone_color >> 16 & 0xFF;
	int stl_g = src->graticule_skintone_color >> 8 & 0xFF;
	int stl_r = src->graticule_skintone_color & 0xFF;
	switch(src->colorspace_calc) {
		case 1: // BT.601
			stl_u = RGB2U_601(stl_r, stl_g, stl_b);
			stl_v = RGB2V_601(stl_r, stl_g, stl_b);
			break;
		default: // BT.709
			stl_u = RGB2U_709(stl_r, stl_g, stl_b);
			stl_v = RGB2V_709(stl_r, stl_g, stl_b);
			break;
	}
	stl_norm = hypotf(stl_u-128.0f, stl_v-128.0f);
	if (stl_norm > 1.0f) {
		stl_u = (stl_u-128.0f) * 128.f/stl_norm + 128.0f;
		stl_v = (stl_v-128.0f) * 128.f/stl_norm + 128.0f;
		gs_vertex2f(128.0f, 128.0f);
		gs_vertex2f(stl_u, 255.f-stl_v);
	}

	// boxes and skin tone line
	src->graticule_line_vbuf = gs_render_save();

	obs_leave_graphics();
}

static void vss_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct vss_source *src = data;
	PROFILE_START(src->bypass_vectorscope ? prof_render_name_b : prof_render_name);

	if (src->update_graticule || src->colorspace_calc<1) {
		src->colorspace_calc = src->colorspace;
		src->update_graticule = 0;
		if (src->colorspace_calc<1 || 2<src->colorspace_calc) {
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi)) {
				switch (ovi.colorspace) {
					case VIDEO_CS_601:
						src->colorspace_calc = 1;
						break;
					case VIDEO_CS_709:
					default:
						src->colorspace_calc = 2;
						break;
				}
			}
		}
		gs_vertexbuffer_destroy(src->graticule_vbuf);
		src->graticule_vbuf = NULL;
		gs_vertexbuffer_destroy(src->graticule_line_vbuf);
		src->graticule_line_vbuf = NULL;
	}

	vss_render_target(src);

	if (src->bypass_vectorscope) {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_texture_t *tex = gs_texrender_get_texture(src->texrender);
		if (!tex)
			goto end;
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(tex, 0, src->known_width, src->known_height);
		}
		goto end;
	}

	PROFILE_START(prof_draw_name);
	if (src->tex_vs) {
		gs_effect_t *effect = vss_effect ? vss_effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_vs);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "intensity"), (float)src->intensity);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(src->tex_vs, 0, VS_SIZE, VS_SIZE);
		}
	}
	PROFILE_END(prof_draw_name);

	PROFILE_START(prof_draw_graticule_name);
	if (src->graticule_img.loaded && src->graticule) {
		create_graticule_vbuf(src);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		draw_uv_vbuffer(src->graticule_vbuf, src->graticule_img.texture, effect, N_GRATICULES*2);
	}

	if (src->graticule && src->graticule_line_vbuf) {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0x8000FF00); // green
		gs_load_vertexbuffer(src->graticule_line_vbuf);
		while (gs_effect_loop(effect, "Solid")) {
			gs_draw(GS_LINES, 0, 0);
		}
	}
	PROFILE_END(prof_draw_graticule_name);

end:;
	PROFILE_END(src->bypass_vectorscope ? prof_render_name_b : prof_render_name);
}

static void vss_tick(void *data, float unused)
{
	UNUSED_PARAMETER(unused);
	struct vss_source *src = data;

	pthread_mutex_lock(&src->target_update_mutex);
	if (src->target_name && !*src->target_name) {
		if (src->weak_target)
			obs_weak_source_release(src->weak_target);
		src->weak_target = NULL;
	}
	if (is_preview_name(src->target_name)) {
		obs_source_t *target = obs_frontend_get_current_preview_scene();
		if (src->weak_target)
			obs_weak_source_release(src->weak_target);
		src->weak_target = target ? obs_source_get_weak_source(target) : NULL;
		obs_source_release(target);
	}
	else if (src->target_name && *src->target_name && !src->weak_target && src->target_check_time) {
		uint64_t t = os_gettime_ns();
		if (t - src->target_check_time > SOURCE_CHECK_NS) {
			src->target_check_time = t;
			obs_source_t *target = obs_get_source_by_name(src->target_name);
			src->weak_target = target ? obs_source_get_weak_source(target) : NULL;
			obs_source_release(target);
		}
	}
	pthread_mutex_unlock(&src->target_update_mutex);

	src->rendered = 0;
}

struct obs_source_info colormonitor_vectorscope = {
	.id = "vectorscope_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = vss_get_name,
	.create = vss_create,
	.destroy = vss_destroy,
	.update = vss_update,
	.get_defaults = vss_get_defaults,
	.get_properties = vss_get_properties,
	.get_width = vss_get_width,
	.get_height = vss_get_height,
	.enum_active_sources = vss_enum_sources,
	.video_render = vss_render,
	.video_tick = vss_tick,
};
