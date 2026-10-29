#pragma once

#include <QList>
#include <QString>

namespace murasaki {

// A file inside a pet item's folder that the importer can turn into a pet but the item check leaves alone (`frame archive not imported`, `mdaa package not
// imported`, `video source not imported`): the user's "Import this file", or the daemon's automatic import when the setting says so.
struct ImportableFile {
    QString path;      // canonical, a regular file inside the item folder
    QString name;      // the file name
    QString kind;      // "zip", "mdaa" or "video" (the importer for videos arrives with the video source)
    qint64 size = 0;
    qint64 modified = 0; // milliseconds since the epoch, with the size what makes a file "the same" for the automatic import
};

// At most 50 files, found in the folder and one level below it. Symlinks that leave the folder are skipped. Opens nothing.
QList<ImportableFile> findImportableFiles(const QString &itemDir);

} // namespace murasaki
