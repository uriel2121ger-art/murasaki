#include "webpwriter.h"

#include "petimport.h"

#include <QByteArray>
#include <QIODevice>

#ifdef MURASAKI_HAVE_WEBPMUX
#include <webp/encode.h>
#include <webp/mux.h>
#endif

#include <cstring>

namespace murasaki::import {

#ifdef MURASAKI_HAVE_WEBPMUX

struct WebpWriter::Impl {
    WebPAnimEncoder *encoder = nullptr;
    WebPConfig config;
};

namespace {
constexpr long long kMaxWebpRawBytes = 2LL << 30; // the encoder holds the frames in memory until the end
constexpr int kMaxWebpSide = 16383;
}

bool WebpWriter::webpOutputAvailable()
{
    return true;
}

WebpWriter::WebpWriter(QIODevice *device, int quality, long long maxBytes) : m_device(device), m_quality(quality), m_maxBytes(maxBytes) {}

WebpWriter::~WebpWriter()
{
    if (m_impl) {
        if (m_impl->encoder)
            WebPAnimEncoderDelete(m_impl->encoder);
        delete m_impl;
    }
}

bool WebpWriter::fail(const QString &message)
{
    m_dead = true;
    if (m_error.isEmpty())
        m_error = message;
    return false;
}

bool WebpWriter::begin(int width, int height)
{
    if (m_begun || m_dead)
        return false;
    if (width < 1 || height < 1 || width > kMaxCanvasSide || height > kMaxCanvasSide || width > kMaxWebpSide || height > kMaxWebpSide)
        return fail(QStringLiteral("canvas %1 x %2 is outside 1..%3").arg(width).arg(height).arg(kMaxCanvasSide));
    if (!m_device || !m_device->isWritable())
        return fail(QStringLiteral("cannot write the output"));
    m_impl = new Impl;
    if (!WebPConfigInit(&m_impl->config))
        return fail(QStringLiteral("libwebp version mismatch"));
    m_impl->config.lossless = m_quality >= 100 ? 1 : 0;
    m_impl->config.quality = m_quality >= 100 ? 75.0f : float(m_quality); // lossless: effort; lossy: quality
    m_impl->config.method = 4;
    m_impl->config.exact = 1;
    if (!WebPValidateConfig(&m_impl->config))
        return fail(QStringLiteral("webp settings refused"));
    WebPAnimEncoderOptions options;
    if (!WebPAnimEncoderOptionsInit(&options))
        return fail(QStringLiteral("libwebpmux version mismatch"));
    options.allow_mixed = 0;
    m_impl->encoder = WebPAnimEncoderNew(width, height, &options);
    if (!m_impl->encoder)
        return fail(QStringLiteral("webp encoder not created"));
    m_width = width;
    m_height = height;
    m_begun = true;
    return true;
}

bool WebpWriter::addFrame(const uchar *rgba, qsizetype stride, int delayMs)
{
    if (m_dead || !m_begun)
        return false;
    if (!rgba || stride < qsizetype(m_width) * 4)
        return fail(QStringLiteral("frame data shorter than the canvas"));
    if (m_frames >= kMaxFrames)
        return fail(QStringLiteral("more than %1 frames").arg(kMaxFrames));
    m_rawHeld += (long long)m_width * m_height * 4;
    if (m_rawHeld > kMaxWebpRawBytes || m_rawHeld > m_maxBytes * 8) {
        m_overBudget = true;
        return fail(QStringLiteral("output over the budget"));
    }
    WebPPicture picture;
    if (!WebPPictureInit(&picture))
        return fail(QStringLiteral("libwebp version mismatch"));
    picture.use_argb = 1;
    picture.width = m_width;
    picture.height = m_height;
    if (!WebPPictureImportRGBA(&picture, rgba, int(stride))) {
        WebPPictureFree(&picture);
        return fail(QStringLiteral("webp picture not built"));
    }
    const bool added = WebPAnimEncoderAdd(m_impl->encoder, &picture, int(m_timestamp), &m_impl->config) != 0;
    WebPPictureFree(&picture);
    if (!added)
        return fail(QStringLiteral("webp encoder refused a frame"));
    m_timestamp += qMax(delayMs, 1);
    ++m_frames;
    return true;
}

bool WebpWriter::finish()
{
    if (m_dead || !m_begun || m_frames < 1)
        return fail(QStringLiteral("no frames"));
    if (!WebPAnimEncoderAdd(m_impl->encoder, nullptr, int(m_timestamp), nullptr))
        return fail(QStringLiteral("webp encoder did not finish"));
    WebPData data;
    WebPDataInit(&data);
    if (!WebPAnimEncoderAssemble(m_impl->encoder, &data)) {
        WebPDataClear(&data);
        return fail(QStringLiteral("webp assemble failed"));
    }
    const long long size = (long long)data.size;
    bool ok = true;
    if (size > m_maxBytes) {
        m_overBudget = true;
        ok = fail(QStringLiteral("output over the budget"));
    } else if (m_device->write(reinterpret_cast<const char *>(data.bytes), qint64(data.size)) != qint64(data.size)) {
        ok = fail(QStringLiteral("cannot write the output"));
    } else {
        m_written = size;
    }
    WebPDataClear(&data);
    return ok;
}

#else // no libwebpmux

struct WebpWriter::Impl {};

bool WebpWriter::webpOutputAvailable() { return false; }
WebpWriter::WebpWriter(QIODevice *device, int quality, long long maxBytes) : m_device(device), m_quality(quality), m_maxBytes(maxBytes) {}
WebpWriter::~WebpWriter() = default;
bool WebpWriter::fail(const QString &message)
{
    m_dead = true;
    m_error = message;
    return false;
}
bool WebpWriter::begin(int, int) { return fail(QStringLiteral("this build has no animated WebP output")); }
bool WebpWriter::addFrame(const uchar *, qsizetype, int) { return false; }
bool WebpWriter::finish() { return false; }

#endif

} // namespace murasaki::import
