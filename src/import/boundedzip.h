#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

class QIODevice;

namespace murasaki::import {

struct ZipLimits {
    int maxEntries = 20000 + 64;
    long long maxMemberBytes = 1LL << 30;   // one member, uncompressed
    long long maxMemoryBytes = 64LL << 20;  // the most read() keeps in memory (bigger members are streamed with extractTo() or sha256())
    long long maxTotalBytes = 2LL << 30;    // all members, uncompressed (as declared; each read is capped by its own declared size)
    int maxRatio = 200;                     // uncompressed / compressed, per member (small members, under 4 KiB, are exempt)
    int maxNameBytes = 512;
    long long maxDirectoryBytes = 32LL << 20; // the central directory itself
    // A member over maxMemberBytes or maxRatio makes the whole archive refused (the default), or, with this set, only that member unreadable (tooBig):
    // an importer that wants frames can ignore a big video or a repetitive text file stored beside them.
    bool skipOversize = false;
};

struct ZipEntry {
    QString name;               // normalised: '/' separators, never absolute, never with '..'
    bool isDirectory = false;
    quint16 method = 0;         // 0 stored, 8 deflate
    quint32 compressedSize = 0;
    quint32 size = 0;
    quint32 crc = 0;
    quint32 localOffset = 0;
    qint64 dataOffset = 0;      // where the member's bytes start, from its local header (checked against this record at open)
    bool tooBig = false;        // over the member or ratio limit and skipped (ZipLimits::skipOversize); read(), extractTo() and sha256() refuse it
};

// A ZIP reader for untrusted files (docs/pets/import.md, "Bounded ZIP"). It reads the central directory once and checks it against the file; it never
// writes anything and never turns a member name into a path. A member is read whole into memory, inflated with a hard cap at its declared size.
//
// Refused at open (error() names it): no end-of-central-directory record exactly at the end (a comment that hides another record is a trick), several
// disks, ZIP64 markers, a central directory outside the file or over its size limit, a local header that disagrees with its directory record (signature,
// name, method, flags), more entries than the limit, an entry that is not a well-formed record, an entry with a
// bad name (empty, over the limit, NUL or a control character, absolute, a drive letter, a ".." component, a duplicate after normalisation and case
// folding), encrypted members, methods other than stored and deflate, symbolic links (Unix attributes), sizes over the limits, a ratio over the limit,
// data that overlaps another member or the central directory. Directories are listed (isDirectory) and skipped by the callers.
class BoundedZip
{
public:
    // `device` must stay alive and seekable. nullopt with *error set (a name of petimport.h followed by ": " and the detail).
    static std::optional<BoundedZip> open(QIODevice *device, const ZipLimits &limits, QString *error);

    const QList<ZipEntry> &entries() const { return m_entries; }
    // The member's bytes (at most maxMemoryBytes). Checks the local header against the directory, inflates with a cap at the declared size, and verifies
    // the size and the CRC-32. nullopt with *error.
    std::optional<QByteArray> read(int index, QString *error) const;
    // The first `bytes` bytes of a member (fewer when it is shorter), to look at its type without inflating all of it. The size and CRC are only
    // verified when the whole member is read, so the caller extracts it later before trusting it.
    std::optional<QByteArray> head(int index, qsizetype bytes, QString *error) const;
    // The same checks, streaming into `sink` (a file the caller opened): memory stays at two 64 KiB buffers whatever the member's size.
    bool extractTo(int index, QIODevice *sink, QString *error) const;
    // The member's bytes in order, as they inflate, to `sink` (which returns false to stop). The same checks as extractTo; the size and CRC are verified at the end.
    bool streamTo(int index, const std::function<bool(const char *, qsizetype)> &sink, QString *error) const { return stream(index, sink, error); }
    // The SHA-256 of a member's bytes, streamed and discarded (the program of a package is hashed and never kept).
    std::optional<QByteArray> sha256(int index, QString *error) const;

private:
    bool stream(int index, const std::function<bool(const char *, qsizetype)> &sink, QString *error) const;

    QIODevice *m_device = nullptr;
    ZipLimits m_limits;
    QList<ZipEntry> m_entries;
    qint64 m_directoryOffset = 0;
};

// The refusal rule for a member name, on its own so that a test and the .mdaa importer use the same one: the normalised name, or an empty string with
// *why set. Does not look at what the name means.
QString normaliseZipName(const QByteArray &raw, bool utf8, int maxBytes, QString *why);

} // namespace murasaki::import
