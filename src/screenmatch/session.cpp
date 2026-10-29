#include "session.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki::screenmatch {

namespace {

QByteArray ok(const QString &word)
{
    return formatReply({.type = ReplyType::Ok, .word = word});
}

QByteArray err(const QString &word, const QString &reason)
{
    return formatReply({.type = ReplyType::Err, .word = word, .reason = reason});
}

int bytesPerPixel(ScreenPixelFormat f)
{
    switch (f) {
    case ScreenPixelFormat::Grey8: return 1;
    case ScreenPixelFormat::Rgb24: return 3;
    case ScreenPixelFormat::Rgba32:
    case ScreenPixelFormat::Bgra32: return 4;
    }
    return 0;
}

bool insideFrame(const Frame &f, int x, int y, int w, int h)
{
    const int bpp = bytesPerPixel(f.format);
    if (!f.data || bpp == 0 || x < 0 || y < 0 || w < 2 || h < 2 || qint64(x) + w > f.width || qint64(y) + h > f.height)
        return false;
    // the last byte the rectangle touches must be inside the buffer
    const qint64 last = qint64(y + h - 1) * f.stride + qint64(x + w) * bpp;
    return last <= f.size && f.stride >= qint64(f.width) * bpp;
}

} // namespace

Session::Session(FrameSource *source, SessionOptions options) : m_source(source), m_options(options) {}

Session::Trigger *Session::find(const QString &id)
{
    for (Trigger &t : m_triggers)
        if (t.id == id)
            return &t;
    return nullptr;
}

int Session::totalRegions() const
{
    int n = 0;
    for (const Trigger &t : m_triggers)
        for (const Region &r : t.regions)
            n += r.set;
    return n;
}

QList<QByteArray> Session::handle(const QByteArray &line, qint64 nowMs)
{
    QString why;
    const auto command = parseCommand(line, &why);
    if (!command) {
        // The word is echoed only when it is one of ours; whatever else was sent is never repeated.
        const QByteArray first = line.left(line.indexOf(' ') < 0 ? line.size() : line.indexOf(' '));
        QString word = u"line"_s;
        for (int t = 0; t <= int(CommandType::Quit); ++t)
            if (commandWord(CommandType(t)) == QString::fromLatin1(first))
                word = commandWord(CommandType(t));
        return {err(word, why)};
    }
    const Command &c = *command;
    const QString word = commandWord(c.type);
    switch (c.type) {
    case CommandType::Clear:
        m_triggers.clear();
        m_pets.clear();
        return {ok(word)};
    case CommandType::Trigger: {
        if (find(c.trigger))
            return {err(word, u"duplicate"_s)};
        if (int(m_triggers.size()) >= kMaxTriggers)
            return {err(word, u"too-many"_s)};
        Trigger t;
        t.id = c.trigger;
        t.mode = c.mode;
        m_triggers.push_back(std::move(t));
        return {ok(word)};
    }
    case CommandType::Region: {
        Trigger *t = find(c.trigger);
        if (!t)
            return {err(word, u"unknown-trigger"_s)};
        if (!t->regions[size_t(c.region)].set && totalRegions() >= kMaxRegionsTotal)
            return {err(word, u"too-many"_s)};
        t->regions[size_t(c.region)] = {true, c.stream, c.x, c.y, c.w, c.h};
        return {ok(word)};
    }
    case CommandType::Ref: {
        Trigger *t = find(c.trigger);
        if (!t)
            return {err(word, u"unknown-trigger"_s)};
        t->refs[size_t(c.region)][size_t(c.step)] = screenPictureFromGrid(reinterpret_cast<const uchar *>(c.grid.constData()));
        t->haveRef[size_t(c.region)][size_t(c.step)] = true;
        return {ok(word)};
    }
    case CommandType::Want: {
        Trigger *t = find(c.trigger);
        if (!t)
            return {err(word, u"unknown-trigger"_s)};
        t->want = c.step;
        return {ok(word)};
    }
    case CommandType::Pets:
        m_pets = c.pets;
        return {ok(word)};
    case CommandType::Rate:
        m_rate = c.rate;
        return {ok(word)};
    case CommandType::Start: {
        const bool was = m_running;
        m_running = true;
        if (!was)
            m_watching = false;
        QList<QByteArray> out{ok(word)};
        if (!was)
            out.append(formatReply({.type = ReplyType::State, .word = u"waiting"_s}));
        return out;
    }
    case CommandType::Stop: {
        const bool was = m_running;
        m_running = false;
        m_watching = false;
        QList<QByteArray> out{ok(word)};
        if (was)
            out.append(formatReply({.type = ReplyType::State, .word = u"stopped"_s}));
        return out;
    }
    case CommandType::Capture:
        return capture(c, nowMs);
    case CommandType::Ping:
        return {formatReply({.type = ReplyType::Pong})};
    case CommandType::Quit:
        m_quit = true;
        m_running = false;
        return {ok(word)};
    }
    return {err(word, u"unhandled"_s)};
}

bool Session::covered(const Region &region) const
{
    if (m_pets.isEmpty())
        return false;
    // A 16 x 16 grid of sample points over the region: more than half of them under a pet rectangle of the same stream and the region is not looked at this tick.
    constexpr int N = 16;
    int hit = 0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const double px = region.x + (i + 0.5) * region.w / N, py = region.y + (j + 0.5) * region.h / N;
            for (const PetRect &p : m_pets) {
                if (p.stream == region.stream && px >= p.x && px < p.x + p.w && py >= p.y && py < p.y + p.h) {
                    ++hit;
                    break;
                }
            }
        }
    return hit * 2 > N * N;
}

QList<QByteArray> Session::tick(qint64)
{
    QList<QByteArray> out;
    if (!m_running)
        return out;
    const int streams = std::min(m_source->streamCount(), kMaxStreams);
    std::array<bool, kMaxStreams> have{};
    std::array<Frame, kMaxStreams> frames{};
    for (int s = 0; s < streams; ++s) {
        bool used = false;
        for (const Trigger &t : m_triggers)
            for (const Region &r : t.regions)
                used = used || (r.set && r.stream == s);
        if (used)
            have[size_t(s)] = m_source->acquire(s, &frames[size_t(s)]);
    }
    const bool any = std::any_of(have.begin(), have.end(), [](bool b) { return b; });
    if (any && !m_watching) {
        m_watching = true;
        out.append(formatReply({.type = ReplyType::State, .word = u"watching"_s}));
    }

    // A crop shared by several triggers is prepared once.
    struct Key {
        int stream, x, y, w, h;
        bool structure;
        bool operator==(const Key &o) const { return stream == o.stream && x == o.x && y == o.y && w == o.w && h == o.h && structure == o.structure; }
    };
    std::vector<std::pair<Key, ScreenPicture>> cache;
    const auto live = [&](const Region &r, bool structure) -> const ScreenPicture * {
        const Key key{r.stream, r.x, r.y, r.w, r.h, structure};
        for (const auto &entry : cache)
            if (entry.first == key)
                return &entry.second;
        const Frame &f = frames[size_t(r.stream)];
        const int bpp = bytesPerPixel(f.format);
        const uchar *origin = f.data + qint64(r.y) * f.stride + qint64(r.x) * bpp;
        const qint64 available = f.size - (origin - f.data);
        cache.emplace_back(key, prepareScreenPictureFromPixels(origin, qsizetype(available), r.w, r.h, f.stride, f.format, structure, screenRowStepFor(r.h)));
        return &cache.back().second;
    };

    for (const Trigger &t : m_triggers) {
        Reply scores{.type = ReplyType::Scores, .trigger = t.id, .step = t.want};
        int lastSet = -1;
        for (int ri = 0; ri < kMaxRegionsPerTrigger; ++ri)
            if (t.regions[size_t(ri)].set)
                lastSet = ri;
        for (int ri = 0; ri <= lastSet; ++ri) { // (slot ri of the line is region ri; a region that was not declared is `-`)
            const Region &r = t.regions[size_t(ri)];
            int permille = -1;
            if (!r.set) {
                scores.permille.append(permille);
                continue;
            }
            if (r.stream < streams && have[size_t(r.stream)] && insideFrame(frames[size_t(r.stream)], r.x, r.y, r.w, r.h) && !covered(r) && t.haveRef[size_t(ri)][size_t(t.want)]) {
                const ScreenPicture *pic = live(r, t.mode == ScreenMatchMode::Structure);
                if (pic->valid)
                    permille = int(std::lround(screenScore(t.mode, t.refs[size_t(ri)][size_t(t.want)], *pic) * 1000.0));
            }
            scores.permille.append(permille);
        }
        if (!scores.permille.isEmpty())
            out.append(formatReply(scores));
    }
    ++m_tick;
    out.append(formatReply({.type = ReplyType::Tick, .tick = m_tick}));
    for (int s = 0; s < streams; ++s)
        if (have[size_t(s)])
            m_source->release(s);
    return out;
}

QByteArray scaledRgbCrop(const Frame &frame, int x, int y, int width, int height, int *outWidth, int *outHeight)
{
    if (!insideFrame(frame, x, y, width, height))
        return {};
    int dw = width, dh = height;
    if (std::max(width, height) > kScreenMatchMaxSide) {
        const double k = double(kScreenMatchMaxSide) / std::max(width, height);
        dw = std::max(1, int(std::lround(width * k)));
        dh = std::max(1, int(std::lround(height * k)));
    }
    const int bpp = bytesPerPixel(frame.format);
    const auto pixel = [&](int px, int py, int channel) -> double {
        const uchar *p = frame.data + qint64(y + py) * frame.stride + qint64(x + px) * bpp;
        switch (frame.format) {
        case ScreenPixelFormat::Grey8: return p[0];
        case ScreenPixelFormat::Rgb24:
        case ScreenPixelFormat::Rgba32: return p[channel];
        case ScreenPixelFormat::Bgra32: return p[2 - channel];
        }
        return 0;
    };
    QByteArray out(qsizetype(dw) * dh * 3, 0);
    for (int j = 0; j < dh; ++j) {
        const int y0 = int(std::floor(double(j) * height / dh)), y1 = std::max(y0 + 1, int(std::ceil(double(j + 1) * height / dh)));
        for (int i = 0; i < dw; ++i) {
            const int x0 = int(std::floor(double(i) * width / dw)), x1 = std::max(x0 + 1, int(std::ceil(double(i + 1) * width / dw)));
            for (int c = 0; c < 3; ++c) {
                double sum = 0;
                int n = 0;
                for (int py = y0; py < std::min(y1, height); ++py)
                    for (int px = x0; px < std::min(x1, width); ++px, ++n)
                        sum += pixel(px, py, c);
                out[qsizetype(j) * dw * 3 + i * 3 + c] = char(std::clamp(int(sum / std::max(1, n) + 0.5), 0, 255));
            }
        }
    }
    *outWidth = dw;
    *outHeight = dh;
    return out;
}

QList<QByteArray> Session::capture(const Command &c, qint64 nowMs)
{
    const QString word = commandWord(CommandType::Capture);
    if (!m_options.captureAllowed)
        return {err(word, u"not-allowed"_s)};
    if (m_lastCaptureMs >= 0 && nowMs - m_lastCaptureMs < m_options.captureMinGapMs)
        return {err(word, u"too-soon"_s)};
    if (c.stream >= m_source->streamCount())
        return {err(word, u"no-stream"_s)};
    Frame frame;
    if (!m_source->acquire(c.stream, &frame))
        return {err(word, u"no-frame"_s)};
    int w = 0, h = 0;
    const QByteArray rgb = scaledRgbCrop(frame, c.x, c.y, c.w, c.h, &w, &h);
    m_source->release(c.stream);
    if (rgb.isEmpty())
        return {err(word, u"outside"_s)};
    m_lastCaptureMs = nowMs;
    QList<QByteArray> out;
    out.append(formatReply({.type = ReplyType::Shot, .request = c.request, .width = w, .height = h, .bytes = int(rgb.size())}));
    int seq = 0;
    for (qsizetype at = 0; at < rgb.size(); at += kChunkBytes)
        out.append(formatReply({.type = ReplyType::Chunk, .request = c.request, .sequence = seq++, .data = rgb.mid(at, kChunkBytes)}));
    out.append(formatReply({.type = ReplyType::ShotEnd, .request = c.request}));
    out.append(ok(word));
    return out;
}

} // namespace murasaki::screenmatch
