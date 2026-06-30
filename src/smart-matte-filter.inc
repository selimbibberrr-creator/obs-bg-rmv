void uploadManualMask(SmartMatteState &state)
{
	if (!state.maskDirty.exchange(false, std::memory_order_acq_rel))
		return;
	QImage snapshot = state.maskSnapshot();
	if (!state.manualTexture) {
		const uint8_t *data[] = {snapshot.constBits()};
		state.manualTexture = gs_texture_create(kMaskWidth, kMaskHeight, GS_RGBA, 1, data, GS_DYNAMIC);
	} else {
		gs_texture_set_image(state.manualTexture, snapshot.constBits(), static_cast<uint32_t>(snapshot.bytesPerLine()), false);
	}
}

void setEffectParameters(SmartMatteState &state, uint32_t width, uint32_t height, gs_texture_t *previous)
{
	uploadManualMask(state);
	if (state.manualParam)
		gs_effect_set_texture(state.manualParam, state.manualTexture);
	if (state.previousParam && previous)
		gs_effect_set_texture(state.previousParam, previous);
	if (state.useHistoryParam)
		gs_effect_set_bool(state.useHistoryParam, previous != nullptr && state.historyValid);
	if (state.texelSizeParam) {
		vec2 texel;
		vec2_set(&texel, 1.0f / std::max(1u, width), 1.0f / std::max(1u, height));
		gs_effect_set_vec2(state.texelSizeParam, &texel);
	}
	if (state.modeParam)
		gs_effect_set_int(state.modeParam, state.mode.load());
	if (state.thresholdParam)
		gs_effect_set_float(state.thresholdParam, state.threshold.load());
	if (state.featherParam)
		gs_effect_set_float(state.featherParam, state.feather.load());
	if (state.edgeShiftParam)
		gs_effect_set_float(state.edgeShiftParam, state.edgeShift.load());
	if (state.centerProtectParam)
		gs_effect_set_float(state.centerProtectParam, state.centerProtect.load());
	if (state.spatialParam)
		gs_effect_set_float(state.spatialParam, state.spatialSmoothing.load());
	if (state.temporalParam)
		gs_effect_set_float(state.temporalParam, state.temporalSmoothing.load());
	if (state.motionParam)
		gs_effect_set_float(state.motionParam, state.motionSensitivity.load());
	if (state.despillParam)
		gs_effect_set_float(state.despillParam, state.despill.load());
	if (state.invertParam)
		gs_effect_set_bool(state.invertParam, state.invert.load());
	if (state.previewParam)
		gs_effect_set_int(state.previewParam, state.preview.load());
}

void resetHistory(SmartMatteState &state, uint32_t width, uint32_t height)
{
	if (state.historyWidth == width && state.historyHeight == height)
		return;
	for (auto *render : state.history) {
		if (render)
			gs_texrender_reset(render);
	}
	state.historyWidth = width;
	state.historyHeight = height;
	state.historyValid = false;
	state.historyIndex = 0;
}

void smartMatteRender(void *data, gs_effect_t *)
{
	auto *context = static_cast<FilterContext *>(data);
	if (!context || !context->state || !context->state->effect) {
		return;
	}
	SmartMatteState &state = *context->state;
	obs_source_t *target = obs_filter_get_target(state.context);
	if (!target) {
		obs_source_skip_video_filter(state.context);
		return;
	}
	const uint32_t width = obs_source_get_base_width(target);
	const uint32_t height = obs_source_get_base_height(target);
	if (!width || !height) {
		obs_source_skip_video_filter(state.context);
		return;
	}

	resetHistory(state, width, height);
	if (!obs_source_process_filter_begin(state.context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	const int writeIndex = 1 - state.historyIndex;
	gs_texrender_t *writeTarget = state.history[writeIndex];
	gs_texture_t *previous = state.historyValid ? gs_texrender_get_texture(state.history[state.historyIndex]) : nullptr;
	gs_texrender_reset(writeTarget);
	if (!gs_texrender_begin(writeTarget, width, height)) {
		obs_source_skip_video_filter(state.context);
		return;
	}

	vec4 clearColor = {};
	gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
	setEffectParameters(state, width, height, previous);
	obs_source_process_filter_end(state.context, state.effect, width, height);
	gs_texrender_end(writeTarget);

	gs_texture_t *result = gs_texrender_get_texture(writeTarget);
	if (result) {
		gs_effect_t *drawEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(drawEffect, "image");
		gs_effect_set_texture(image, result);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(drawEffect, "Draw"))
			gs_draw_sprite(result, 0, width, height);
		gs_blend_state_pop();
		state.historyIndex = writeIndex;
		state.historyValid = true;
	}
}

void smartMatteUpdate(void *data, obs_data_t *settings)
{
	auto *context = static_cast<FilterContext *>(data);
	if (!context || !context->state)
		return;
	auto &state = *context->state;
	state.mode.store(static_cast<int>(obs_data_get_int(settings, S_MODE)));
	state.threshold.store(static_cast<float>(obs_data_get_double(settings, S_THRESHOLD)));
	state.feather.store(static_cast<float>(obs_data_get_double(settings, S_FEATHER)));
	state.edgeShift.store(static_cast<float>(obs_data_get_double(settings, S_EDGE_SHIFT)));
	state.centerProtect.store(static_cast<float>(obs_data_get_double(settings, S_CENTER_PROTECT)));
	state.spatialSmoothing.store(static_cast<float>(obs_data_get_double(settings, S_SPATIAL)));
	state.temporalSmoothing.store(static_cast<float>(obs_data_get_double(settings, S_TEMPORAL)));
	state.motionSensitivity.store(static_cast<float>(obs_data_get_double(settings, S_MOTION)));
	state.despill.store(static_cast<float>(obs_data_get_double(settings, S_DESPILL)));
	state.invert.store(obs_data_get_bool(settings, S_INVERT));
	state.preview.store(static_cast<int>(obs_data_get_int(settings, S_PREVIEW)));
}

void destroyGraphics(SmartMatteState &state)
{
	obs_enter_graphics();
	if (state.effect)
		gs_effect_destroy(state.effect);
	if (state.manualTexture)
		gs_texture_destroy(state.manualTexture);
	for (auto *&render : state.history) {
		if (render)
			gs_texrender_destroy(render);
		render = nullptr;
	}
	obs_leave_graphics();
	state.effect = nullptr;
	state.manualTexture = nullptr;
}

void smartMatteDestroy(void *data)
{
	auto *context = static_cast<FilterContext *>(data);
	if (!context)
		return;
	if (context->state) {
		context->state->alive.store(false);
		context->state->saveMask();
		registryRemove(context->state->instanceId);
		destroyGraphics(*context->state);
	}
	delete context;
}

void *smartMatteCreate(obs_data_t *settings, obs_source_t *source)
{
	auto context = std::make_unique<FilterContext>();
	context->state = std::make_shared<SmartMatteState>();
	auto &state = *context->state;
	state.context = source;
	state.displayName = obs_source_get_name(source) ? obs_source_get_name(source) : "OBS Smart Matte";

	const char *storedId = obs_data_get_string(settings, S_INSTANCE_ID);
	std::string id = storedId ? storedId : "";
	if (id.empty() || registryHasId(id)) {
		id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
		obs_data_set_string(settings, S_INSTANCE_ID, id.c_str());
	}
	state.instanceId = id;
	state.loadMask();

	char *effectPath = obs_module_file("effects/smart_matte.effect");
	char *effectError = nullptr;
	obs_enter_graphics();
	state.effect = gs_effect_create_from_file(effectPath, &effectError);
	state.history[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	state.history[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (state.effect) {
		state.manualParam = gs_effect_get_param_by_name(state.effect, "manual_mask");
		state.previousParam = gs_effect_get_param_by_name(state.effect, "previous_image");
		state.useHistoryParam = gs_effect_get_param_by_name(state.effect, "use_history");
		state.texelSizeParam = gs_effect_get_param_by_name(state.effect, "texel_size");
		state.modeParam = gs_effect_get_param_by_name(state.effect, "matte_mode");
		state.thresholdParam = gs_effect_get_param_by_name(state.effect, "threshold");
		state.featherParam = gs_effect_get_param_by_name(state.effect, "feather");
		state.edgeShiftParam = gs_effect_get_param_by_name(state.effect, "edge_shift");
		state.centerProtectParam = gs_effect_get_param_by_name(state.effect, "center_protect");
		state.spatialParam = gs_effect_get_param_by_name(state.effect, "spatial_smoothing");
		state.temporalParam = gs_effect_get_param_by_name(state.effect, "temporal_smoothing");
		state.motionParam = gs_effect_get_param_by_name(state.effect, "motion_sensitivity");
		state.despillParam = gs_effect_get_param_by_name(state.effect, "despill");
		state.invertParam = gs_effect_get_param_by_name(state.effect, "invert_mask");
		state.previewParam = gs_effect_get_param_by_name(state.effect, "preview_mode");
	}
	obs_leave_graphics();
	bfree(effectPath);

	if (!state.effect || !state.history[0] || !state.history[1]) {
		blog(LOG_ERROR, "[OBS Smart Matte] Shader initialization failed: %s", effectError ? effectError : "unknown error");
		bfree(effectError);
		destroyGraphics(state);
		return nullptr;
	}
	bfree(effectError);

	smartMatteUpdate(context.get(), settings);
	registryAdd(context->state);
	blog(LOG_INFO, "[OBS Smart Matte] Filter instance %s created", state.instanceId.c_str());
	return context.release();
}

bool openEditorButton(obs_properties_t *, obs_property_t *, void *data)
{
	auto *context = static_cast<FilterContext *>(data);
	if (g_editor && context && context->state)
		g_editor->selectState(context->state);
	return true;
}

bool clearMaskButton(obs_properties_t *, obs_property_t *, void *data)
{
	auto *context = static_cast<FilterContext *>(data);
	if (context && context->state)
		context->state->clearMask();
	return true;
}

obs_properties_t *smartMatteProperties(void *data)
{
	auto *props = obs_properties_create();
	obs_properties_add_text(props, "about", obs_module_text("Filter.Info"), OBS_TEXT_INFO);

	auto *mode = obs_properties_add_list(props, S_MODE, obs_module_text("Filter.Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Filter.Mode.AutoManual"), 0);
	obs_property_list_add_int(mode, obs_module_text("Filter.Mode.Manual"), 1);
	obs_property_list_add_int(mode, obs_module_text("Filter.Mode.Auto"), 2);

	obs_properties_add_float_slider(props, S_THRESHOLD, obs_module_text("Filter.Threshold"), 0.01, 0.80, 0.005);
	obs_properties_add_float_slider(props, S_FEATHER, obs_module_text("Filter.Feather"), 0.0, 0.35, 0.005);
	obs_properties_add_float_slider(props, S_EDGE_SHIFT, obs_module_text("Filter.EdgeShift"), -0.25, 0.25, 0.005);
	obs_properties_add_float_slider(props, S_CENTER_PROTECT, obs_module_text("Filter.CenterProtect"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_SPATIAL, obs_module_text("Filter.Spatial"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_TEMPORAL, obs_module_text("Filter.Temporal"), 0.0, 0.95, 0.01);
	obs_properties_add_float_slider(props, S_MOTION, obs_module_text("Filter.Motion"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_DESPILL, obs_module_text("Filter.Despill"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(props, S_INVERT, obs_module_text("Filter.Invert"));

	auto *preview = obs_properties_add_list(props, S_PREVIEW, obs_module_text("Filter.Preview"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preview, obs_module_text("Filter.Preview.Final"), 0);
	obs_property_list_add_int(preview, obs_module_text("Filter.Preview.Alpha"), 1);
	obs_property_list_add_int(preview, obs_module_text("Filter.Preview.Auto"), 2);
	obs_property_list_add_int(preview, obs_module_text("Filter.Preview.Manual"), 3);

	if (data) {
		obs_properties_add_button(props, "open_editor", obs_module_text("Filter.OpenEditor"), openEditorButton);
		obs_properties_add_button(props, "clear_mask", obs_module_text("Filter.ClearMask"), clearMaskButton);
	}
	return props;
}

void smartMatteDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MODE, 0);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.16);
	obs_data_set_default_double(settings, S_FEATHER, 0.08);
	obs_data_set_default_double(settings, S_EDGE_SHIFT, 0.0);
	obs_data_set_default_double(settings, S_CENTER_PROTECT, 0.22);
	obs_data_set_default_double(settings, S_SPATIAL, 0.35);
	obs_data_set_default_double(settings, S_TEMPORAL, 0.38);
	obs_data_set_default_double(settings, S_MOTION, 0.55);
	obs_data_set_default_double(settings, S_DESPILL, 0.20);
	obs_data_set_default_bool(settings, S_INVERT, false);
	obs_data_set_default_int(settings, S_PREVIEW, 0);
}

const char *smartMatteName(void *)
{
	return obs_module_text("Filter.Name");
}

obs_source_info makeSourceInfo()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = smartMatteName;
	info.create = smartMatteCreate;
	info.destroy = smartMatteDestroy;
	info.update = smartMatteUpdate;
	info.video_render = smartMatteRender;
	info.get_properties = smartMatteProperties;
	info.get_defaults = smartMatteDefaults;
	return info;
}

obs_source_info g_sourceInfo = makeSourceInfo();

} // namespace

bool obs_module_load(void)
{
	obs_register_source(&g_sourceInfo);
	g_editor = new MaskEditorWidget;
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Editor.Title"), g_editor)) {
		blog(LOG_WARNING, "[OBS Smart Matte] Could not add editor dock");
		delete g_editor;
		g_editor = nullptr;
	}
	blog(LOG_INFO, "[OBS Smart Matte] Version 0.3.0 loaded");
	return true;
}

void obs_module_unload(void)
{
	if (g_editor) {
		QPointer<MaskEditorWidget> editor = g_editor;
		obs_frontend_remove_dock(kDockId);
		if (editor)
			delete editor;
		g_editor = nullptr;
	}
	blog(LOG_INFO, "[OBS Smart Matte] Unloaded");
}
