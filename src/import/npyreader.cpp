#include "npyreader.h"

#include "boundedzip.h"
#include "petimport.h"

#include <QIODevice>

#include <algorithm>

namespace murasaki::import {

namespace {

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

constexpr char kMagic[] = "\x93NUMPY";
constexpr int kMagicSize = 6;

// The header's dictionary, by its fixed grammar:
//   { 'descr' : '<string>' , 'fortran_order' : True|False , 'shape' : ( n , n , ... ) , }
// keys in any order, each exactly once, single or double quotes, spaces anywhere between tokens, an optional trailing comma. Nothing else is accepted.
struct DictParser {
    const QByteArray &text;
    qsizetype pos = 0;
    QString problem;

    bool bad(const QString &why)
    {
        if (problem.isEmpty())
            problem = why;
        return false;
    }
    void skip()
    {
        while (pos < text.size() && text.at(pos) == ' ')
            ++pos;
    }
    bool take(char c)
    {
        skip();
        if (pos < text.size() && text.at(pos) == c) {
            ++pos;
            return true;
        }
        return bad(QStringLiteral("expected '%1' at byte %2").arg(QLatin1Char(c)).arg(pos));
    }
    bool peek(char c)
    {
        skip();
        return pos < text.size() && text.at(pos) == c;
    }
    bool string(QString *out)
    {
        skip();
        if (pos >= text.size() || (text.at(pos) != '\'' && text.at(pos) != '"'))
            return bad(QStringLiteral("expected a string at byte %1").arg(pos));
        const char quote = text.at(pos++);
        const qsizetype start = pos;
        while (pos < text.size() && text.at(pos) != quote) {
            if (text.at(pos) == '\\')
                return bad(QStringLiteral("an escape in a string"));
            ++pos;
        }
        if (pos >= text.size())
            return bad(QStringLiteral("a string that does not end"));
        *out = QString::fromLatin1(text.constData() + start, pos - start);
        ++pos;
        return true;
    }
    bool integer(long long *out)
    {
        skip();
        const qsizetype start = pos;
        long long v = 0;
        while (pos < text.size() && text.at(pos) >= '0' && text.at(pos) <= '9' && pos - start < 12) {
            v = v * 10 + (text.at(pos) - '0');
            ++pos;
        }
        if (pos == start)
            return bad(QStringLiteral("expected a number at byte %1").arg(pos));
        if (pos - start > 1 && text.at(start) == '0')
            return bad(QStringLiteral("a number with a leading zero"));
        if (pos < text.size() && text.at(pos) >= '0' && text.at(pos) <= '9')
            return bad(QStringLiteral("a number with more than 12 digits"));
        *out = v;
        return true;
    }
    bool keyword(const char *word)
    {
        skip();
        const qsizetype n = qsizetype(qstrlen(word));
        if (text.mid(pos, n) != QByteArrayView(word, n))
            return bad(QStringLiteral("expected %1 at byte %2").arg(QLatin1String(word)).arg(pos));
        pos += n;
        return true;
    }
};

struct Dict {
    QString descr;
    bool fortran = false;
    QList<long long> shape;
};

bool parseDict(const QByteArray &text, Dict *out, QString *problem)
{
    DictParser p{text, 0, {}};
    bool haveDescr = false, haveOrder = false, haveShape = false;
    const auto done = [&]() {
        *problem = p.problem;
        return false;
    };
    if (!p.take('{'))
        return done();
    while (true) {
        if (p.peek('}'))
            break;
        QString key;
        if (!p.string(&key) || !p.take(':'))
            return done();
        if (key == QLatin1String("descr") && !haveDescr) {
            haveDescr = true;
            if (!p.string(&out->descr))
                return done();
        } else if (key == QLatin1String("fortran_order") && !haveOrder) {
            haveOrder = true;
            p.skip();
            if (p.pos < text.size() && text.at(p.pos) == 'T') {
                if (!p.keyword("True"))
                    return done();
                out->fortran = true;
            } else if (!p.keyword("False")) {
                return done();
            }
        } else if (key == QLatin1String("shape") && !haveShape) {
            haveShape = true;
            if (!p.take('('))
                return done();
            while (!p.peek(')')) {
                long long n = 0;
                if (!p.integer(&n))
                    return done();
                if (out->shape.size() >= 8) {
                    p.bad(QStringLiteral("more than 8 dimensions"));
                    return done();
                }
                out->shape.append(n);
                if (!p.peek(','))
                    break;
                p.take(',');
            }
            if (!p.take(')'))
                return done();
        } else {
            p.bad(QStringLiteral("key \"%1\" is repeated or not one of descr, fortran_order, shape").arg(key.left(30)));
            return done();
        }
        if (p.peek(',')) {
            p.take(',');
            continue;
        }
        break;
    }
    if (!p.take('}'))
        return done();
    p.skip();
    if (p.pos != text.size()) {
        p.bad(QStringLiteral("data after the closing brace"));
        return done();
    }
    if (!haveDescr || !haveOrder || !haveShape) {
        p.bad(QStringLiteral("a key of descr, fortran_order and shape is missing"));
        return done();
    }
    return true;
}

} // namespace

std::optional<NpyHeader> parseNpyHeader(const QByteArray &prefix, const NpyLimits &limits, QString *error, bool *needMore)
{
    const auto refuse = [&](std::string_view name, const QString &detail = {}) -> std::optional<NpyHeader> {
        if (error)
            *error = named(name, detail);
        return std::nullopt;
    };
    const auto more = [&]() -> std::optional<NpyHeader> {
        if (needMore)
            *needMore = true;
        return std::nullopt;
    };
    if (needMore)
        *needMore = false;
    const qsizetype known = std::min<qsizetype>(prefix.size(), kMagicSize);
    if (prefix.left(known) != QByteArrayView(kMagic, known))
        return refuse(kNpyNotNpy);
    if (prefix.size() < 8)
        return more();
    const int major = uchar(prefix.at(6)), minor = uchar(prefix.at(7));
    if (minor != 0 || (major != 1 && major != 2 && major != 3))
        return refuse(kNpyVersion, QStringLiteral("%1.%2").arg(major).arg(minor));
    const int lengthBytes = major == 1 ? 2 : 4;
    if (prefix.size() < 8 + lengthBytes)
        return more();
    quint32 headerLength = 0;
    for (int i = 0; i < lengthBytes; ++i)
        headerLength |= quint32(uchar(prefix.at(8 + i))) << (8 * i);
    if (headerLength > quint32(limits.maxHeaderBytes))
        return refuse(kNpyHeaderTooLarge, QStringLiteral("%1 bytes").arg(headerLength));
    if (headerLength == 0)
        return refuse(kNpyHeaderMalformed, QStringLiteral("an empty header"));
    const qsizetype dataOffset = 8 + lengthBytes + qsizetype(headerLength);
    if (prefix.size() < dataOffset)
        return more();

    QByteArray text = prefix.mid(8 + lengthBytes, qsizetype(headerLength));
    if (text.back() != '\n')
        return refuse(kNpyHeaderMalformed, QStringLiteral("the header does not end with a newline"));
    text.chop(1);
    for (const char c : text)
        if (uchar(c) < 0x20 || uchar(c) > 0x7e)
            return refuse(kNpyHeaderMalformed, QStringLiteral("a control or non-ASCII byte"));
    Dict dict;
    QString problem;
    if (!parseDict(text, &dict, &problem))
        return refuse(kNpyHeaderMalformed, problem);

    if (dict.descr != QLatin1String("|u1") && dict.descr != QLatin1String("<u1"))
        return refuse(kNpyDtype, dict.descr.left(30).contains(QLatin1Char('O')) ? QStringLiteral("an object array") : dict.descr.left(30));
    if (dict.fortran)
        return refuse(kNpyFortranOrder);
    if (dict.shape.size() != 4)
        return refuse(kNpyShape, QStringLiteral("%1 dimensions").arg(dict.shape.size()));
    for (const long long d : dict.shape)
        if (d <= 0)
            return refuse(kNpyShape, QStringLiteral("a dimension is zero"));
    if (dict.shape.at(3) != 3)
        return refuse(kNpyShape, QStringLiteral("the last dimension is %1").arg(dict.shape.at(3)));
    // Each dimension against its own limit first, so that the product below cannot overflow.
    if (dict.shape.at(0) > limits.maxFrames || dict.shape.at(1) > limits.maxSide || dict.shape.at(2) > limits.maxSide)
        return refuse(kNpyOverLimit, QStringLiteral("shape %1 x %2 x %3").arg(dict.shape.at(0)).arg(dict.shape.at(1)).arg(dict.shape.at(2)));
    NpyHeader header;
    header.shape.frames = int(dict.shape.at(0));
    header.shape.height = int(dict.shape.at(1));
    header.shape.width = int(dict.shape.at(2));
    header.dataOffset = dataOffset;
    if (header.shape.dataBytes() > limits.maxDataBytes)
        return refuse(kNpyOverLimit, QStringLiteral("%1 MiB of data").arg(header.shape.dataBytes() >> 20));
    return header;
}

NpyFrameReader::NpyFrameReader(const NpyLimits &limits, Choose choose, Deliver deliver, long long knownSize)
    : m_limits(limits), m_choose(std::move(choose)), m_deliver(std::move(deliver)), m_knownSize(knownSize)
{
}

bool NpyFrameReader::fail(const QString &why)
{
    if (!m_failed) {
        m_failed = true;
        m_error = why;
    }
    return false;
}

bool NpyFrameReader::feed(const char *data, qsizetype length)
{
    if (m_failed)
        return false;
    if (m_alive && !m_alive())
        return fail(named(kCancelled));
    if (!m_haveHeader) {
        m_head.append(data, length);
        QString why;
        bool needMore = false;
        const auto header = parseNpyHeader(m_head, m_limits, &why, &needMore);
        if (!header)
            return needMore ? true : fail(why);
        m_shape = header->shape;
        m_haveHeader = true;
        if (m_knownSize >= 0) {
            const long long need = header->dataOffset + m_shape.dataBytes();
            if (m_knownSize < need)
                return fail(named(kNpyTruncated, QStringLiteral("the shape needs %1 bytes, the file has %2").arg(need).arg(m_knownSize)));
            if (m_knownSize > need)
                return fail(named(kNpyTrailingData, QStringLiteral("%1 bytes").arg(m_knownSize - need)));
        }
        m_wanted = m_choose ? m_choose(m_shape) : QList<int>();
        std::sort(m_wanted.begin(), m_wanted.end());
        m_wanted.erase(std::unique(m_wanted.begin(), m_wanted.end()), m_wanted.end());
        if (!m_wanted.isEmpty() && (m_wanted.first() < 0 || m_wanted.last() >= m_shape.frames))
            return fail(named(kNpyShape, QStringLiteral("frame %1 asked of an array of %2").arg(m_wanted.last()).arg(m_shape.frames)));
        if (!m_wanted.isEmpty())
            m_frame.resize(qsizetype(m_shape.frameBytes()));
        const QByteArray rest = m_head.mid(header->dataOffset);
        m_head.clear();
        if (rest.isEmpty())
            return true;
        data = rest.constData();
        length = rest.size();
        return feed(data, length); // m_haveHeader is set: the rest is data
    }

    const long long frameBytes = m_shape.frameBytes(), total = m_shape.dataBytes();
    while (length > 0) {
        if (m_consumed >= total)
            return fail(named(kNpyTrailingData));
        const long long index = m_consumed / frameBytes, offset = m_consumed % frameBytes;
        const qsizetype chunk = qsizetype(std::min<long long>({length, frameBytes - offset, total - m_consumed}));
        if (m_nextWanted < m_wanted.size() && m_wanted.at(m_nextWanted) == int(index)) {
            std::copy(data, data + chunk, m_frame.data() + offset);
            if (offset + chunk == frameBytes) {
                ++m_nextWanted;
                if (!m_deliver(int(index), reinterpret_cast<const uchar *>(m_frame.constData()), m_shape.width, m_shape.height))
                    return fail(named(kCancelled));
            }
        }
        m_consumed += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

bool NpyFrameReader::finish()
{
    if (m_failed)
        return false;
    if (!m_haveHeader)
        return fail(m_head.isEmpty() ? named(kNpyNotNpy, QStringLiteral("empty")) : named(kNpyTruncated, QStringLiteral("the header is incomplete")));
    if (m_consumed < m_shape.dataBytes())
        return fail(named(kNpyTruncated, QStringLiteral("%1 of %2 bytes").arg(m_consumed).arg(m_shape.dataBytes())));
    return true;
}

bool readNpyFrames(QIODevice *device, const NpyLimits &limits, const NpyFrameReader::Choose &choose, const NpyFrameReader::Deliver &deliver, QString *error, NpyShape *shape,
                   const std::function<bool()> &alive)
{
    const long long known = device->isSequential() ? -1 : device->size() - device->pos();
    NpyFrameReader reader(limits, choose, deliver, known);
    reader.setAlive(alive);
    QByteArray buffer(64 * 1024, 0);
    bool ok = true;
    while (ok) {
        const qint64 n = device->read(buffer.data(), buffer.size());
        if (n < 0) {
            ok = false;
            if (error)
                *error = named(kNpyTruncated, QStringLiteral("the file could not be read"));
            return false;
        }
        if (n == 0)
            break;
        ok = reader.feed(buffer.constData(), qsizetype(n));
    }
    ok = ok && reader.finish();
    if (reader.hasShape() && shape)
        *shape = reader.shape();
    if (!ok && error)
        *error = reader.error();
    return ok;
}

bool readNpzFrames(QIODevice *device, const NpyLimits &limits, const NpyFrameReader::Choose &choose, const NpyFrameReader::Deliver &deliver, QString *error, NpyShape *shape,
                   const std::function<bool()> &alive)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    ZipLimits zl;
    zl.maxEntries = 16;
    zl.maxMemberBytes = limits.maxDataBytes + limits.maxHeaderBytes + 64;
    zl.maxTotalBytes = zl.maxMemberBytes * 2;
    zl.maxRatio = 20000; // a flat picture deflates a very long way; the memory used is one frame whatever the ratio
    zl.maxNameBytes = 64;
    QString why;
    const auto zip = BoundedZip::open(device, zl, &why);
    if (!zip)
        return refuse(why.startsWith(named(kZipNotAZip)) ? named(kNpzNotAZip, why.mid(named(kZipNotAZip).size()).remove(0, 2)) : why);
    int index = -1;
    for (int i = 0; i < zip->entries().size(); ++i)
        if (!zip->entries().at(i).isDirectory && zip->entries().at(i).name == QLatin1String("frames.npy"))
            index = i;
    if (index < 0)
        return refuse(named(kNpzNoFrames));
    NpyFrameReader reader(limits, choose, deliver, zip->entries().at(index).size);
    reader.setAlive(alive);
    const bool streamed = zip->streamTo(index, [&](const char *data, qsizetype length) { return reader.feed(data, length); }, &why);
    if (reader.hasShape() && shape)
        *shape = reader.shape();
    if (!reader.error().isEmpty())
        return refuse(reader.error());
    if (!streamed)
        return refuse(why);
    if (!reader.finish())
        return refuse(reader.error());
    return true;
}

} // namespace murasaki::import
