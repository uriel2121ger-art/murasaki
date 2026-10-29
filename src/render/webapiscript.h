#pragma once

#include <QJsonObject>
#include <QString>

namespace murasaki {

// Wallpaper Engine's applyUserProperties payload, {key: {value}}, for every property except labels and groups.
// The value is the property's value, overridden by the item's presetValues, overridden by overrides (the
// assignment's properties); null entries do not override.
QJsonObject webUserProperties(const QJsonObject &item, const QJsonObject &overrides);

// JS injected at document creation into a web item's main world. It defines Wallpaper Engine's page globals
// (the audio listener gets silence for now) and window.__murasaki.start(fps) / setPaused(bool) / setFps(fps) /
// applyProperties(obj), which WebItem calls after the page loads, whenever playback pauses or resumes, and when
// the FPS limit or the user properties change.
QString buildWebApiScript(const QJsonObject &item, const QJsonObject &overrides);

} // namespace murasaki
