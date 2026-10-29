#pragma once

#include <QJsonObject>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace murasaki {

inline constexpr QLatin1String kItemScheme("murasaki-item");

// What one output shows, derived from the daemon's ResolveAssignment reply.
struct WallpaperContent {
    // Changes whenever the delegate must be recreated (assignment except `muted`, or the item itself).
    // Empty when nothing is assigned.
    QString key;
    QString kind;        // "none" (black), "video", "web", "scene" or "preview"
    QString itemId;
    QString contentDir;
    QString entryFile;   // absolute
    QString previewPath; // absolute, or empty
    QString fill = QStringLiteral("fill");
    bool muted = false;
    // The assignment spreads one wallpaper across all outputs (its key changes with it, as the delegate does).
    bool span = false;
    QString script;      // web items only: buildWebApiScript()
    // Web items only: webUserProperties() with the assignment's overrides applied. Not part of `key`, so the
    // surface can push changes into the running page.
    QJsonObject userProperties;
    // Web items only: canonical paths the page may read through file:// URLs (its dirs, and files or dirs the
    // user picked for its properties).
    QStringList readableRoots;
    // Web items only: the user allowed this item to use the internet (permissions.network of the item). Part of `key`
    // through the item JSON, so toggling it reloads the page under its new rules.
    bool networkAllowed = false;
    // Scene items only: Wallpaper Engine's "assets" directory (the scene engine needs it), and the resolved user
    // properties as {key: value} (colors as "r g b" strings). Not part of `key`, like userProperties.
    QString assetsDir;
    QJsonObject sceneProperties;
    // Why a scene shows its preview instead (no Wallpaper Engine assets); empty otherwise.
    QString reason;

    // `assetsDir` is the daemon's WallpaperEngineAssetsDir; scenes cannot play without it.
    static WallpaperContent fromResolveReply(const QJsonObject &reply, const QString &assetsDir = {});
};

// The entries of `after` ({key: {value}}) whose value differs from `before`, or that `before` lacks.
QJsonObject changedUserProperties(const QJsonObject &before, const QJsonObject &after);

// murasaki-item://<token>/<entryFile relative to contentDir>; invalid when the token is empty or entryFile is not
// inside contentDir.
QUrl itemEntryUrl(const QString &token, const QString &contentDir, const QString &entryFile);

} // namespace murasaki
