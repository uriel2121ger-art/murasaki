#pragma once

// The reference pictures of the screen triggers: <data>/murasaki/triggers/<trigger id>/region<r>/step<k>.png,
// with <data> from XDG_DATA_HOME. Only the frames a trigger uses are kept, each a PNG scaled to a longer side of at most 360, and a deleted trigger deletes its folder. Plain files
// and bytes, no Qt GUI: a picture is checked from its PNG header only and handed back as bytes, so whoever decodes one must cap the decode (QImageReader allocation limit, size after
// decoding) and not trust the header. Writes never follow or write through a link (a temporary file made with O_EXCL and O_NOFOLLOW, fsync, then rename).

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace murasaki {

inline constexpr int kMaxScreenRefRegions = 64;
inline constexpr int kMaxScreenRefSteps = 64;     // reference frames per region
inline constexpr int kMaxScreenRefSide = 360;
inline constexpr qsizetype kMaxScreenRefBytes = 4 << 20;

class ScreenRefStore
{
public:
    // `root` is the folder that holds the trigger folders; the default is <GenericDataLocation>/murasaki/triggers (XDG_DATA_HOME respected).
    explicit ScreenRefStore(const QString &root = defaultRoot());

    static QString defaultRoot();
    // 1 to 64 of A-Z a-z 0-9 _ -: nothing that can name a path.
    static bool validTriggerId(const QString &id);

    const QString &root() const { return m_root; }
    QString triggerFolder(const QString &id) const; // empty for an invalid id

    // Writes one picture atomically (a temporary file renamed over the target). The bytes must be a PNG whose header says at most 360 pixels on its longer side and at most
    // kMaxScreenRefBytes long. False with *error for an invalid id, region or step, a bad picture, or a failed write.
    bool putFrame(const QString &id, int region, int step, const QByteArray &png, QString *error = nullptr);
    QByteArray frame(const QString &id, int region, int step) const; // empty when there is none
    QList<int> steps(const QString &id, int region) const;           // the steps present, ascending
    QList<int> regions(const QString &id) const;                      // the regions present, ascending

    // The data kept beside the pictures (which clip frame each step is, and so on); a small JSON object, written atomically.
    bool putInfo(const QString &id, const QJsonObject &info, QString *error = nullptr);
    QJsonObject info(const QString &id) const;

    // Takes the pictures an import left in its job folder: the `screenRefs` record of the importer lists files "screenrefs/region<r>/step<k>.png" under `importFolder`. Every file is
    // checked like putFrame's. All or nothing, also for an id that exists: the pictures are built in a staging folder and swapped in whole (the old ones, with any step or region the new import
    // does not have, go with them), and a refusal leaves the trigger as it was. Returns the number of pictures kept.
    int adoptImported(const QString &id, const QString &importFolder, const QJsonObject &screenRefs, QString *error = nullptr);

    // Removes the pictures of one region of a trigger (a new set of references replaces the old one whole). True when they are gone.
    bool removeRegion(const QString &id, int region);
    // Removes the trigger's folder and everything in it. True when it is gone (also when it never existed). Refuses an invalid id.
    bool removeTrigger(const QString &id);
    QStringList triggerIds() const;

private:
    bool ensureFolder(const QString &path, QString *error) const;

    QString m_root;
};

// The size a PNG header declares (the IHDR chunk), or {0, 0} when the bytes do not begin like a PNG.
struct ScreenRefPngSize {
    int width = 0;
    int height = 0;
};
ScreenRefPngSize screenRefPngSize(const QByteArray &png);

} // namespace murasaki
