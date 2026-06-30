// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QEnterEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-smart-matte", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Realtime adaptive background removal with a non-destructive manual mask editor.";
}

namespace {

constexpr const char *kFilterId = "obs_smart_matte_filter";
constexpr const char *kDockId = "obs_smart_matte_editor";
constexpr int kMaskWidth = 512;
constexpr int kMaskHeight = 512;
constexpr int kMaxUndoSteps = 30;

constexpr const char *S_INSTANCE_ID = "instance_id";
constexpr const char *S_MODE = "mode";
constexpr const char *S_THRESHOLD = "threshold";
constexpr const char *S_FEATHER = "feather";
constexpr const char *S_EDGE_SHIFT = "edge_shift";
constexpr const char *S_CENTER_PROTECT = "center_protect";
constexpr const char *S_SPATIAL = "spatial_smoothing";
constexpr const char *S_TEMPORAL = "temporal_smoothing";
constexpr const char *S_MOTION = "motion_sensitivity";
constexpr const char *S_DESPILL = "despill";
constexpr const char *S_INVERT = "invert";
constexpr const char *S_PREVIEW = "preview";

class MaskEditorWidget;
MaskEditorWidget *g_editor = nullptr;

struct SmartMatteState {
	obs_source_t *context = nullptr;
	std::string instanceId;
	std::string displayName;

	gs_effect_t *effect = nullptr;
	gs_texture_t *manualTexture = nullptr;
	gs_texrender_t *history[2] = {nullptr, nullptr};
	int historyIndex = 0;
	bool historyValid = false;
	uint32_t historyWidth = 0;
	uint32_t historyHeight = 0;

	gs_eparam_t *manualParam = nullptr;
	gs_eparam_t *previousParam = nullptr;
	gs_eparam_t *useHistoryParam = nullptr;
	gs_eparam_t *texelSizeParam = nullptr;
	gs_eparam_t *modeParam = nullptr;
	gs_eparam_t *thresholdParam = nullptr;
	gs_eparam_t *featherParam = nullptr;
	gs_eparam_t *edgeShiftParam = nullptr;
	gs_eparam_t *centerProtectParam = nullptr;
	gs_eparam_t *spatialParam = nullptr;
	gs_eparam_t *temporalParam = nullptr;
	gs_eparam_t *motionParam = nullptr;
	gs_eparam_t *despillParam = nullptr;
	gs_eparam_t *invertParam = nullptr;
	gs_eparam_t *previewParam = nullptr;

	std::atomic<int> mode{0};
	std::atomic<float> threshold{0.16f};
	std::atomic<float> feather{0.08f};
	std::atomic<float> edgeShift{0.0f};
	std::atomic<float> centerProtect{0.22f};
	std::atomic<float> spatialSmoothing{0.35f};
	std::atomic<float> temporalSmoothing{0.38f};
	std::atomic<float> motionSensitivity{0.55f};
	std::atomic<float> despill{0.20f};
	std::atomic<bool> invert{false};
	std::atomic<int> preview{0};
	std::atomic<bool> alive{true};

	mutable std::mutex maskMutex;
	QImage mask = QImage(kMaskWidth, kMaskHeight, QImage::Format_RGBA8888);
	std::atomic<bool> maskDirty{true};

	SmartMatteState()
	{
		mask.fill(Qt::transparent);
	}

	QString maskPath() const
	{
		const std::string relative = "masks/" + instanceId + ".png";
		char *path = obs_module_config_path(relative.c_str());
		if (!path)
			return {};
		const QString result = QString::fromUtf8(path);
		bfree(path);
		return result;
	}

	void loadMask()
	{
		const QString path = maskPath();
		if (path.isEmpty() || !QFileInfo::exists(path))
			return;

		QImage loaded(path);
		if (loaded.isNull()) {
			blog(LOG_WARNING, "[OBS Smart Matte] Could not load mask: %s", path.toUtf8().constData());
			return;
		}

		loaded = loaded.convertToFormat(QImage::Format_RGBA8888)
				 .scaled(kMaskWidth, kMaskHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		{
			std::lock_guard lock(maskMutex);
			mask = loaded;
		}
		maskDirty.store(true, std::memory_order_release);
	}

	void saveMask() const
	{
		const QString path = maskPath();
		if (path.isEmpty())
			return;

		QDir().mkpath(QFileInfo(path).absolutePath());
		QImage snapshot;
		{
			std::lock_guard lock(maskMutex);
			snapshot = mask.copy();
		}
		if (!snapshot.save(path, "PNG"))
			blog(LOG_WARNING, "[OBS Smart Matte] Could not save mask: %s", path.toUtf8().constData());
	}

	QImage maskSnapshot() const
	{
		std::lock_guard lock(maskMutex);
		return mask.copy();
	}

	void setMask(const QImage &newMask)
	{
		QImage converted = newMask.convertToFormat(QImage::Format_RGBA8888);
		if (converted.size() != QSize(kMaskWidth, kMaskHeight))
			converted = converted.scaled(kMaskWidth, kMaskHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		{
			std::lock_guard lock(maskMutex);
			mask = converted;
		}
		maskDirty.store(true, std::memory_order_release);
	}

	void clearMask()
	{
		QImage cleared(kMaskWidth, kMaskHeight, QImage::Format_RGBA8888);
		cleared.fill(Qt::transparent);
		setMask(cleared);
		saveMask();
	}
};

struct FilterContext {
	std::shared_ptr<SmartMatteState> state;
};

std::mutex g_registryMutex;
std::vector<std::weak_ptr<SmartMatteState>> g_registry;

std::vector<std::shared_ptr<SmartMatteState>> registrySnapshot()
{
	std::vector<std::shared_ptr<SmartMatteState>> result;
	std::lock_guard lock(g_registryMutex);
	g_registry.erase(std::remove_if(g_registry.begin(), g_registry.end(), [](const auto &weak) {
		return weak.expired();
	}), g_registry.end());
	for (const auto &weak : g_registry) {
		if (auto state = weak.lock(); state && state->alive.load())
			result.push_back(std::move(state));
	}
	return result;
}

bool registryHasId(const std::string &id)
{
	for (const auto &state : registrySnapshot()) {
		if (state->instanceId == id)
			return true;
	}
	return false;
}

void registryAdd(const std::shared_ptr<SmartMatteState> &state)
{
	std::lock_guard lock(g_registryMutex);
	g_registry.emplace_back(state);
}

void registryRemove(const std::string &id)
{
	std::lock_guard lock(g_registryMutex);
	g_registry.erase(std::remove_if(g_registry.begin(), g_registry.end(), [&id](const auto &weak) {
		auto state = weak.lock();
		return !state || state->instanceId == id;
	}), g_registry.end());
}

