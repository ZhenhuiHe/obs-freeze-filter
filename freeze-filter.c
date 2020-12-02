#include <obs-module.h>
#include "freeze-filter.h"
#include "version.h"

#include <util/circlebuf.h>

struct freeze_info {
	obs_source_t *source;

	struct circlebuf renders;
	long long max_renders;
	uint32_t cx;
	uint32_t cy;
	bool target_valid;
	bool processed_frame;
	size_t frames_loaded;
	size_t current_frame;
	long long freeze_mode;
	bool backward;
	obs_hotkey_pair_id hotkey;
	float duration;
	uint32_t duration_max;
	uint32_t refresh_interval;
	float last_refresh;

	uint32_t activate_action;
	uint32_t deactivate_action;
	uint32_t show_action;
	uint32_t hide_action;

	uint32_t delayed_action;
	uint64_t start_delay;
	uint64_t end_delay;
	float delay_duration;

	bool mask;
	gs_effect_t *effect;
	float mask_left;
	float mask_right;
	float mask_top;
	float mask_bottom;
	float feathering;

	uint32_t fade_duration;
	float fading;
};

static const char *freeze_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Freeze");
}

static void free_textures(struct freeze_info *f, bool destroy_effect)
{
	if (!f->renders.size && !destroy_effect)
		return;
	obs_enter_graphics();
	while (f->renders.size) {
		gs_texrender_t *render;
		circlebuf_pop_front(&f->renders, &render,
				    sizeof(gs_texrender_t *));
		gs_texrender_destroy(render);
	}
	if (destroy_effect) {
		gs_effect_destroy(f->effect);
		f->effect = NULL;
	}
	obs_leave_graphics();
}

static inline bool check_size(struct freeze_info *f)
{
	obs_source_t *target = obs_filter_get_target(f->source);

	f->target_valid = !!target;
	if (!f->target_valid)
		return true;

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);

	f->target_valid = !!cx && !!cy;
	if (!f->target_valid)
		return true;

	if (cx != f->cx || cy != f->cy) {
		f->cx = cx;
		f->cy = cy;
		free_textures(f, false);
		return true;
	}
	return false;
}

static void freeze_update(void *data, obs_data_t *settings)
{
	struct freeze_info *freeze = data;
	freeze->max_renders = obs_data_get_int(settings, "frames");
	if (freeze->max_renders <= 0)
		freeze->max_renders = 1;
	freeze->freeze_mode = obs_data_get_int(settings, "freeze_mode");
	freeze->duration_max = obs_data_get_int(settings, "duration");
	freeze->refresh_interval =
		obs_data_get_int(settings, "refresh_interval");
	freeze->fade_duration = obs_data_get_int(settings, "fade_duration");
	freeze->activate_action = obs_data_get_int(settings, "activate_action");
	freeze->deactivate_action =
		obs_data_get_int(settings, "deactivate_action");
	freeze->show_action = obs_data_get_int(settings, "show_action");
	freeze->hide_action = obs_data_get_int(settings, "hide_action");
	freeze->start_delay = obs_data_get_int(settings, "start_delay");
	freeze->end_delay = obs_data_get_int(settings, "end_delay");
	freeze->mask = obs_data_get_bool(settings, "mask");
	freeze->mask_left =
		(float)obs_data_get_double(settings, "mask_left") / 100.0f;
	freeze->mask_right =
		(float)obs_data_get_double(settings, "mask_right") / 100.0f;
	freeze->mask_top =
		(float)obs_data_get_double(settings, "mask_top") / 100.0f;
	freeze->mask_bottom =
		(float)obs_data_get_double(settings, "mask_bottom") / 100.0f;
	freeze->feathering =
		(float)obs_data_get_double(settings, "feathering") / 100.0f;
}

static void *freeze_create(obs_data_t *settings, obs_source_t *source)
{
	struct freeze_info *freeze = bzalloc(sizeof(struct freeze_info));
	freeze->source = source;
	freeze->hotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	freeze->fading = -1.0f;
	obs_enter_graphics();
	char *effect_path = obs_module_file("freeze_part.effect");
	freeze->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);

	obs_leave_graphics();
	freeze_update(freeze, settings);
	return freeze;
}

static void freeze_destroy(void *data)
{
	struct freeze_info *freeze = data;
	if (freeze->hotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_hotkey_pair_unregister(freeze->hotkey);
	}
	free_textures(freeze, true);
	bfree(freeze);
}

static void draw_frame(struct freeze_info *f)
{

	if (f->mask ||
	    (f->fading >= 0.0f && f->fading * 1000.0 < f->fade_duration) ||
	    !f->renders.size)
		obs_source_skip_video_filter(f->source);

	if (!f->renders.size || f->frames_loaded < f->max_renders)
		return;

	while (f->renders.size > f->max_renders * sizeof(gs_texrender_t *)) {
		gs_texrender_t *render;
		circlebuf_pop_front(&f->renders, &render,
				    sizeof(gs_texrender_t *));
		gs_texrender_destroy(render);
	}

	const size_t count = f->renders.size / sizeof(gs_texrender_t *);
	size_t frame = f->current_frame;
	if (frame >= count) {
		frame = count - 1;
	}
	gs_texrender_t *render = *(gs_texrender_t **)circlebuf_data(
		&f->renders, frame * sizeof(gs_texrender_t *));

	gs_texture_t *tex = gs_texrender_get_texture(render);
	if (tex) {
		gs_effect_t *effect = f->effect;
		if (!effect)
			effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);
		if (f->effect) {
			gs_eparam_t *p =
				gs_effect_get_param_by_name(effect, "opacity");
			if (f->fading >= 0.0f &&
			    f->fading * 1000.0 < f->fade_duration) {
				gs_effect_set_float(
					p, ((float)f->fade_duration -
					    (f->fading * 1000.0f)) /
						   (float)f->fade_duration);
			} else {
				gs_effect_set_float(p, 1.0f);
			}
			p = gs_effect_get_param_by_name(effect, "maskLeft");
			gs_effect_set_float(p, f->mask ? f->mask_left : 1.0f);
			p = gs_effect_get_param_by_name(effect, "maskRight");
			gs_effect_set_float(p, f->mask ? f->mask_right : 1.0f);
			p = gs_effect_get_param_by_name(effect, "maskTop");
			gs_effect_set_float(p, f->mask ? f->mask_top : 1.0f);
			p = gs_effect_get_param_by_name(effect, "maskBottom");
			gs_effect_set_float(p, f->mask ? f->mask_bottom : 1.0f);
			p = gs_effect_get_param_by_name(effect, "feathering");
			gs_effect_set_float(p, f->feathering);

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_SRCALPHA,
					  GS_BLEND_INVSRCALPHA);
		}
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, f->cx, f->cy);
		if (f->effect)
			gs_blend_state_pop();
	}
}

static void freeze_video_render(void *data, gs_effect_t *effect)
{
	struct freeze_info *freeze = data;
	obs_source_t *target = obs_filter_get_target(freeze->source);
	obs_source_t *parent = obs_filter_get_parent(freeze->source);

	if (!freeze->target_valid || !target || !parent) {
		obs_source_skip_video_filter(freeze->source);
		return;
	}
	if (freeze->processed_frame ||
	    freeze->frames_loaded >= freeze->max_renders) {
		draw_frame(freeze);
		return;
	}
	gs_texrender_t *render = NULL;
	if (freeze->renders.size &&
	    freeze->renders.size >=
		    freeze->max_renders * sizeof(gs_texrender_t *))
		circlebuf_pop_front(&freeze->renders, &render,
				    sizeof(gs_texrender_t *));

	if (!render) {
		render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	} else {
		gs_texrender_reset(render);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(render, freeze->cx, freeze->cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)freeze->cx, 0.0f, (float)freeze->cy,
			 -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(render);
	}

	gs_blend_state_pop();
	circlebuf_push_back(&freeze->renders, &render,
			    sizeof(gs_texrender_t *));
	draw_frame(freeze);
	freeze->processed_frame = true;
	freeze->frames_loaded++;
	if (freeze->freeze_mode == FREEZE_MODE_BACK_FORTH) {
		freeze->current_frame = freeze->frames_loaded;
		freeze->backward = true;
	}
}

static void prop_list_add_actions(obs_property_t *p)
{
	obs_property_list_add_int(p, obs_module_text("None"),
				  FREEZE_ACTION_NONE);
	obs_property_list_add_int(p, obs_module_text("FreezeEnable"),
				  FREEZE_ACTION_ENABLE);
	obs_property_list_add_int(p, obs_module_text("FreezeDisable"),
				  FREEZE_ACTION_DISABLE);
}

static obs_properties_t *freeze_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_int(
		ppts, "frames", obs_module_text("Frames"), 1, 100, 1);
	p = obs_properties_add_int(ppts, "fade_duration",
				   obs_module_text("FadeDuration"), 0, 10000,
				   1000);
	obs_property_int_set_suffix(p, "ms");
	p = obs_properties_add_int(
		ppts, "duration", obs_module_text("Duration"), 0, 100000, 1000);
	obs_property_int_set_suffix(p, "ms");
	p = obs_properties_add_int(ppts, "refresh_interval",
				   obs_module_text("RefreshInterval"), 0,
				   100000, 1000);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_int(ppts, "fade_duration",
				   obs_module_text("FadeDuration"), 0, 10000,
				   1000);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_int(ppts, "start_delay",
				   obs_module_text("StartDelay"), 0, 100000,
				   1000);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_int(ppts, "end_delay",
				   obs_module_text("EndDelay"), 0, 100000,
				   1000);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_list(ppts, "freeze_mode",
				    obs_module_text("FreezeMode"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Random"),
				  FREEZE_MODE_RANDOM);
	obs_property_list_add_int(p, obs_module_text("Loop"), FREEZE_MODE_LOOP);
	obs_property_list_add_int(p, obs_module_text("BackForth"),
				  FREEZE_MODE_BACK_FORTH);

	obs_properties_t *group = obs_properties_create();

	p = obs_properties_add_float_slider(group, "mask_left",
					    obs_module_text("MaskLeft"), 0.0,
					    100.0, .01);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(group, "mask_right",
					    obs_module_text("MaskRight"), 0.0,
					    100.0, .01);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(
		group, "mask_top", obs_module_text("MaskTop"), 0.0, 100.0, .01);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(group, "mask_bottom",
					    obs_module_text("MaskBottom"), 0.0,
					    100.0, .01);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float_slider(group, "feathering",
					    obs_module_text("Feathering"), 0.0,
					    10.0, .01);

	obs_properties_add_group(ppts, "mask", obs_module_text("Mask"),
				 OBS_GROUP_CHECKABLE, group);

	group = obs_properties_create();

	p = obs_properties_add_list(group, "activate_action",
				    obs_module_text("ActivateAction"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_actions(p);
	p = obs_properties_add_list(group, "deactivate_action",
				    obs_module_text("DeactivateAction"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_actions(p);
	p = obs_properties_add_list(group, "show_action",
				    obs_module_text("ShowAction"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_actions(p);
	p = obs_properties_add_list(group, "hide_action",
				    obs_module_text("HideAction"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_actions(p);

	obs_properties_add_group(ppts, "action", obs_module_text("Action"),
				 OBS_GROUP_NORMAL, group);

	return ppts;
}

void freeze_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "frames", 1);
	obs_data_set_default_double(settings, "feathering", 2.0);
}

void freeze_do_action(struct freeze_info *freeze, uint32_t action)
{
	if (action == FREEZE_ACTION_ENABLE &&
	    !obs_source_enabled(freeze->source)) {
		obs_source_set_enabled(freeze->source, true);
	} else if (action == FREEZE_ACTION_DISABLE &&
		   obs_source_enabled(freeze->source)) {
		if (freeze->fade_duration) {
			freeze->fading = 0.0f;
		} else {
			obs_source_set_enabled(freeze->source, false);
		}
	}
}

void freeze_do_or_delay_action(struct freeze_info *freeze, uint32_t action)
{
	if (action == FREEZE_ACTION_NONE)
		return;
	if ((freeze->start_delay && action == FREEZE_ACTION_ENABLE) ||
	    (freeze->end_delay && action == FREEZE_ACTION_DISABLE)) {
		freeze->delay_duration = 0.0f;
		freeze->delayed_action = action;
	} else {
		freeze_do_action(freeze, action);
	}
}

bool freeze_enable_hotkey(void *data, obs_hotkey_pair_id id,
			  obs_hotkey_t *hotkey, bool pressed)
{
	struct freeze_info *freeze = data;
	if (!pressed)
		return false;
	if (obs_source_enabled(freeze->source) && freeze->fading < 0.0f)
		return false;

	if (freeze->fading >= 0.0f)
		freeze->fading = -1.0f;

	if (!obs_source_enabled(freeze->source)) {
		freeze_do_or_delay_action(freeze, FREEZE_ACTION_ENABLE);
	}
	return true;
}

bool freeze_disable_hotkey(void *data, obs_hotkey_pair_id id,
			   obs_hotkey_t *hotkey, bool pressed)
{
	struct freeze_info *freeze = data;
	if (!pressed)
		return false;
	if (!obs_source_enabled(freeze->source) || freeze->fading >= 0.0f)
		return false;
	freeze_do_or_delay_action(freeze, FREEZE_ACTION_DISABLE);
	return true;
}

static void freeze_tick(void *data, float t)
{
	struct freeze_info *f = data;

	if (f->delayed_action != FREEZE_ACTION_NONE) {
		f->delay_duration += t;
		if (f->delay_duration * 1000.0 >=
		    (f->delayed_action == FREEZE_ACTION_ENABLE
			     ? f->start_delay
			     : f->end_delay)) {
			freeze_do_action(f, f->delayed_action);
			f->delayed_action = FREEZE_ACTION_NONE;
		}
	}

	if (f->fading >= 0.0f) {
		f->fading += t;
		if (f->fading * 1000.0 >= f->fade_duration) {
			f->fading = -1.0f;
			obs_source_set_enabled(f->source, false);
		}
	}

	if (obs_source_enabled(f->source)) {
		f->duration += t;
		if (f->duration_max && f->duration * 1000.0 > f->duration_max) {
			if (f->fade_duration) {
				if (f->fading < 0.0f)
					f->fading = 0.0f;
			} else {
				obs_source_set_enabled(f->source, false);
			}
		} else if (f->refresh_interval &&
			   f->duration > f->last_refresh &&
			   (f->duration - f->last_refresh) * 1000.0 >=
				   f->refresh_interval) {
			f->frames_loaded = 0;
			f->last_refresh = f->duration;
		}
	} else {
		f->frames_loaded = 0;
		f->duration = 0.0f;
		f->last_refresh = 0.0f;
	}
	if (f->hotkey == OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_source_t *parent = obs_filter_get_parent(f->source);
		if (parent) {
			f->hotkey = obs_hotkey_pair_register_source(
				parent, "Freeze.Enable",
				obs_module_text("FreezeEnable"),
				"Freeze.Disable",
				obs_module_text("FreezeDisable"),
				freeze_enable_hotkey, freeze_disable_hotkey, f,
				f);
		}
	}
	if (check_size(f))
		f->frames_loaded = 0;
	f->processed_frame = false;

	const size_t count = f->frames_loaded;
	if (count <= 1) {
		f->current_frame = 0;
	} else if (f->freeze_mode == FREEZE_MODE_RANDOM) {
		const size_t r = (size_t)rand();
		f->current_frame = r % count;
	} else if (f->freeze_mode == FREEZE_MODE_LOOP) {
		if (f->current_frame < count - 1) {
			f->current_frame++;
		} else {
			f->current_frame = 0;
		}
	} else if (f->freeze_mode == FREEZE_MODE_BACK_FORTH) {
		if (f->backward) {
			if (f->current_frame > 0) {
				f->current_frame--;
			} else {
				f->backward = false;
				if (f->current_frame < count - 1)
					f->current_frame++;
			}
		} else {
			if (f->current_frame < count - 1) {
				f->current_frame++;
			} else {
				f->backward = true;
				if (f->current_frame > 0)
					f->current_frame--;
			}
		}
	}
}

void freeze_activate(void *data)
{
	struct freeze_info *freeze = data;
	freeze_do_or_delay_action(freeze, freeze->activate_action);
}

void freeze_deactivate(void *data)
{
	struct freeze_info *freeze = data;
	freeze_do_or_delay_action(freeze, freeze->deactivate_action);
}

void freeze_show(void *data)
{
	struct freeze_info *freeze = data;
	freeze_do_or_delay_action(freeze, freeze->show_action);
}

void freeze_hide(void *data)
{
	struct freeze_info *freeze = data;
	freeze_do_or_delay_action(freeze, freeze->hide_action);
}

struct obs_source_info freeze_filter = {
	.id = "freeze_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_OUTPUT_VIDEO,
	.get_name = freeze_get_name,
	.create = freeze_create,
	.destroy = freeze_destroy,
	.load = freeze_update,
	.update = freeze_update,
	.video_render = freeze_video_render,
	.get_properties = freeze_properties,
	.get_defaults = freeze_defaults,
	.video_tick = freeze_tick,
	.activate = freeze_activate,
	.deactivate = freeze_deactivate,
	.show = freeze_show,
	.hide = freeze_hide,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("freeze-filter", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("FreezeFilter");
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Freeze Filter] loaded version %s", PROJECT_VERSION);
	obs_register_source(&freeze_filter);
	return true;
}
