#include "protocol.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace murasaki::screenmatch {

namespace {

bool fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return false;
}

// Splits on single spaces; empty tokens (double, leading or trailing spaces) and bytes outside 0x21..0x7e refuse the line.
bool tokens(const QByteArray &line, QList<QByteArray> *out, QString *error)
{
    if (line.isEmpty())
        return fail(error, u"empty line"_s);
    if (line.size() >= kMaxLineBytes)
        return fail(error, u"line too long"_s);
    qsizetype start = 0;
    for (qsizetype i = 0; i <= line.size(); ++i) {
        if (i < line.size()) {
            const uchar c = uchar(line.at(i));
            if (c != ' ' && (c < 0x21 || c > 0x7e))
                return fail(error, u"not printable ASCII"_s);
        }
        if (i == line.size() || line.at(i) == ' ') {
            if (i == start)
                return fail(error, u"empty word"_s);
            out->append(line.mid(start, i - start));
            start = i + 1;
        }
    }
    return true;
}

bool integer(const QByteArray &t, int low, int high, int *out)
{
    if (t.isEmpty() || t.size() > 9)
        return false;
    for (qsizetype i = 0; i < t.size(); ++i)
        if (t.at(i) < '0' || t.at(i) > '9')
            return false;
    if (t.size() > 1 && t.at(0) == '0')
        return false;
    const int v = t.toInt();
    if (v < low || v > high)
        return false;
    *out = v;
    return true;
}

bool base64(const QByteArray &t, qsizetype exactBytes, qsizetype maxBytes, QByteArray *out)
{
    const auto decoded = QByteArray::fromBase64Encoding(t, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    if (exactBytes >= 0 ? decoded.decoded.size() != exactBytes : decoded.decoded.size() > maxBytes || decoded.decoded.isEmpty())
        return false;
    if (decoded.decoded.toBase64() != t) // one spelling only: padding bits and padding are exactly what encoding gives
        return false;
    *out = decoded.decoded;
    return true;
}

struct Word {
    CommandType type;
    const char *name;
};
constexpr Word kWords[] = {{CommandType::Clear, "clear"}, {CommandType::Trigger, "trigger"}, {CommandType::Region, "region"}, {CommandType::Ref, "ref"},     {CommandType::Want, "want"},
                           {CommandType::Pets, "pets"},   {CommandType::Rate, "rate"},       {CommandType::Start, "start"},   {CommandType::Stop, "stop"},   {CommandType::Capture, "capture"},
                           {CommandType::Ping, "ping"},   {CommandType::Quit, "quit"}};

} // namespace

bool validTriggerId(const QString &id)
{
    static const QRegularExpression pattern(u"\\A[A-Za-z0-9_-]{1,64}\\z"_s);
    return pattern.match(id).hasMatch();
}

QString commandWord(CommandType type)
{
    for (const Word &w : kWords)
        if (w.type == type)
            return QString::fromLatin1(w.name);
    return {};
}

std::optional<Command> parseCommand(const QByteArray &line, QString *error)
{
    QList<QByteArray> t;
    if (!tokens(line, &t, error))
        return std::nullopt;
    Command c;
    bool known = false;
    for (const Word &w : kWords)
        if (t.at(0) == w.name) {
            c.type = w.type;
            known = true;
        }
    if (!known) {
        fail(error, u"unknown command"_s);
        return std::nullopt;
    }
    const auto bad = [&](const QString &why) -> std::optional<Command> {
        fail(error, why);
        return std::nullopt;
    };
    const auto count = [&](int n) { return t.size() == n; };
    const auto id = [&](const QByteArray &token) {
        c.trigger = QString::fromLatin1(token);
        return validTriggerId(c.trigger);
    };
    switch (c.type) {
    case CommandType::Clear:
    case CommandType::Start:
    case CommandType::Stop:
    case CommandType::Ping:
    case CommandType::Quit:
        if (!count(1))
            return bad(u"takes no argument"_s);
        break;
    case CommandType::Trigger:
        if (!count(3) || !id(t.at(1)))
            return bad(u"trigger <id> <mode>"_s);
        if (t.at(2) == "hash")
            c.mode = ScreenMatchMode::Hash;
        else if (t.at(2) == "structure")
            c.mode = ScreenMatchMode::Structure;
        else
            return bad(u"mode is hash or structure"_s);
        break;
    case CommandType::Region:
        if (!count(8) || !id(t.at(1)) || !integer(t.at(2), 0, kMaxRegionsPerTrigger - 1, &c.region) || !integer(t.at(3), 0, kMaxStreams - 1, &c.stream) || !integer(t.at(4), 0, kMaxCoordinate, &c.x)
            || !integer(t.at(5), 0, kMaxCoordinate, &c.y) || !integer(t.at(6), 1, kMaxSide, &c.w) || !integer(t.at(7), 1, kMaxSide, &c.h))
            return bad(u"region <id> <r> <stream> <x> <y> <w> <h>"_s);
        break;
    case CommandType::Ref:
        if (!count(5) || !id(t.at(1)) || !integer(t.at(2), 0, kMaxRegionsPerTrigger - 1, &c.region) || !integer(t.at(3), 0, kMaxSteps - 1, &c.step) || !base64(t.at(4), kGridBytes, kGridBytes, &c.grid))
            return bad(u"ref <id> <r> <step> <base64 of 1024 bytes>"_s);
        break;
    case CommandType::Want:
        if (!count(3) || !id(t.at(1)) || !integer(t.at(2), 0, kMaxSteps - 1, &c.step))
            return bad(u"want <id> <step>"_s);
        break;
    case CommandType::Pets: {
        int n = 0;
        if (t.size() < 2 || !integer(t.at(1), 0, kMaxPets, &n) || t.size() != 2 + 5 * n)
            return bad(u"pets <n> then n rectangles"_s);
        for (int i = 0; i < n; ++i) {
            PetRect r;
            const int at = 2 + 5 * i;
            if (!integer(t.at(at), 0, kMaxStreams - 1, &r.stream) || !integer(t.at(at + 1), 0, kMaxCoordinate, &r.x) || !integer(t.at(at + 2), 0, kMaxCoordinate, &r.y)
                || !integer(t.at(at + 3), 1, kMaxSide, &r.w) || !integer(t.at(at + 4), 1, kMaxSide, &r.h))
                return bad(u"a pet rectangle is out of range"_s);
            c.pets.append(r);
        }
        break;
    }
    case CommandType::Rate:
        if (!count(2) || !integer(t.at(1), kMinRate, kMaxRate, &c.rate))
            return bad(u"rate 1 to 16"_s);
        break;
    case CommandType::Capture:
        if (!count(7) || !integer(t.at(1), 1, 1000000, &c.request) || !integer(t.at(2), 0, kMaxStreams - 1, &c.stream) || !integer(t.at(3), 0, kMaxCoordinate, &c.x) || !integer(t.at(4), 0, kMaxCoordinate, &c.y)
            || !integer(t.at(5), 1, kMaxSide, &c.w) || !integer(t.at(6), 1, kMaxSide, &c.h))
            return bad(u"capture <req> <stream> <x> <y> <w> <h>"_s);
        break;
    }
    return c;
}

QByteArray formatCommand(const Command &c)
{
    const QByteArray word = commandWord(c.type).toLatin1();
    const auto n = [](int v) { return QByteArray::number(v); };
    const QByteArray id = c.trigger.toLatin1();
    switch (c.type) {
    case CommandType::Clear:
    case CommandType::Start:
    case CommandType::Stop:
    case CommandType::Ping:
    case CommandType::Quit:
        return word;
    case CommandType::Trigger:
        return word + ' ' + id + ' ' + (c.mode == ScreenMatchMode::Hash ? "hash" : "structure");
    case CommandType::Region:
        return word + ' ' + id + ' ' + n(c.region) + ' ' + n(c.stream) + ' ' + n(c.x) + ' ' + n(c.y) + ' ' + n(c.w) + ' ' + n(c.h);
    case CommandType::Ref:
        return word + ' ' + id + ' ' + n(c.region) + ' ' + n(c.step) + ' ' + c.grid.toBase64();
    case CommandType::Want:
        return word + ' ' + id + ' ' + n(c.step);
    case CommandType::Pets: {
        QByteArray out = word + ' ' + n(int(c.pets.size()));
        for (const PetRect &r : c.pets)
            out += ' ' + n(r.stream) + ' ' + n(r.x) + ' ' + n(r.y) + ' ' + n(r.w) + ' ' + n(r.h);
        return out;
    }
    case CommandType::Rate:
        return word + ' ' + n(c.rate);
    case CommandType::Capture:
        return word + ' ' + n(c.request) + ' ' + n(c.stream) + ' ' + n(c.x) + ' ' + n(c.y) + ' ' + n(c.w) + ' ' + n(c.h);
    }
    return word;
}

std::optional<Reply> parseReply(const QByteArray &line, QString *error)
{
    QList<QByteArray> t;
    if (!tokens(line, &t, error))
        return std::nullopt;
    Reply r;
    const auto bad = [&](const QString &why) -> std::optional<Reply> {
        fail(error, why);
        return std::nullopt;
    };
    const QByteArray &w = t.at(0);
    if (w == "hello") {
        r.type = ReplyType::Hello;
        if (t.size() != 2 || !integer(t.at(1), 0, 1000, &r.version))
            return bad(u"hello <version>"_s);
    } else if (w == "ok") {
        r.type = ReplyType::Ok;
        if (t.size() != 2)
            return bad(u"ok <word>"_s);
        r.word = QString::fromLatin1(t.at(1));
    } else if (w == "err") {
        r.type = ReplyType::Err;
        if (t.size() < 3)
            return bad(u"err <word> <reason>"_s);
        r.word = QString::fromLatin1(t.at(1));
        QByteArray reason;
        for (int i = 2; i < t.size(); ++i)
            reason += (i > 2 ? " " : "") + t.at(i);
        r.reason = QString::fromLatin1(reason);
    } else if (w == "s") {
        r.type = ReplyType::Scores;
        if (t.size() < 4 || t.size() > 3 + kMaxRegionsPerTrigger || !integer(t.at(2), 0, kMaxSteps - 1, &r.step))
            return bad(u"s <id> <step> <p>..."_s);
        r.trigger = QString::fromLatin1(t.at(1));
        if (!validTriggerId(r.trigger))
            return bad(u"bad trigger id"_s);
        for (int i = 3; i < t.size(); ++i) {
            int p = -1;
            if (t.at(i) != "-" && !integer(t.at(i), 0, 1000, &p))
                return bad(u"bad score"_s);
            r.permille.append(p);
        }
    } else if (w == "t") {
        r.type = ReplyType::Tick;
        int v = 0;
        if (t.size() != 2 || !integer(t.at(1), 0, 2000000000, &v))
            return bad(u"t <n>"_s);
        r.tick = v;
    } else if (w == "state") {
        r.type = ReplyType::State;
        if (t.size() != 2 || (t.at(1) != "waiting" && t.at(1) != "watching" && t.at(1) != "stopped"))
            return bad(u"state waiting|watching|stopped"_s);
        r.word = QString::fromLatin1(t.at(1));
    } else if (w == "shot") {
        r.type = ReplyType::Shot;
        if (t.size() != 5 || !integer(t.at(1), 1, 1000000, &r.request) || !integer(t.at(2), 1, 360, &r.width) || !integer(t.at(3), 1, 360, &r.height) || !integer(t.at(4), 1, kMaxCaptureBytes, &r.bytes)
            || r.bytes != r.width * r.height * 3)
            return bad(u"shot <req> <w> <h> <bytes>"_s);
    } else if (w == "chunk") {
        r.type = ReplyType::Chunk;
        if (t.size() != 4 || !integer(t.at(1), 1, 1000000, &r.request) || !integer(t.at(2), 0, 1000, &r.sequence) || !base64(t.at(3), -1, kChunkBytes, &r.data))
            return bad(u"chunk <req> <seq> <base64>"_s);
    } else if (w == "shotend") {
        r.type = ReplyType::ShotEnd;
        if (t.size() != 2 || !integer(t.at(1), 1, 1000000, &r.request))
            return bad(u"shotend <req>"_s);
    } else if (w == "pong") {
        r.type = ReplyType::Pong;
        if (t.size() != 1)
            return bad(u"pong"_s);
    } else {
        return bad(u"unknown reply"_s);
    }
    return r;
}

QByteArray formatReply(const Reply &r)
{
    const auto n = [](qint64 v) { return QByteArray::number(v); };
    switch (r.type) {
    case ReplyType::Hello:
        return "hello " + n(r.version);
    case ReplyType::Ok:
        return "ok " + r.word.toLatin1();
    case ReplyType::Err:
        return "err " + r.word.toLatin1() + ' ' + r.reason.toLatin1();
    case ReplyType::Scores: {
        QByteArray out = "s " + r.trigger.toLatin1() + ' ' + n(r.step);
        for (const int p : r.permille)
            out += p < 0 ? QByteArray(" -") : ' ' + n(p);
        return out;
    }
    case ReplyType::Tick:
        return "t " + n(r.tick);
    case ReplyType::State:
        return "state " + r.word.toLatin1();
    case ReplyType::Shot:
        return "shot " + n(r.request) + ' ' + n(r.width) + ' ' + n(r.height) + ' ' + n(r.bytes);
    case ReplyType::Chunk:
        return "chunk " + n(r.request) + ' ' + n(r.sequence) + ' ' + r.data.toBase64();
    case ReplyType::ShotEnd:
        return "shotend " + n(r.request);
    case ReplyType::Pong:
        return "pong";
    }
    return {};
}

QList<QByteArray> LineBuffer::feed(const char *data, qsizetype length)
{
    QList<QByteArray> lines;
    for (qsizetype i = 0; i < length; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (m_discarding)
                m_discarding = false;
            else
                lines.append(m_buffer);
            m_buffer.clear();
            continue;
        }
        if (m_discarding)
            continue;
        if (m_buffer.size() >= kMaxLineBytes - 1) { // the newline would be the 4096th byte: this line is too long
            m_buffer.clear();
            m_discarding = true;
            ++m_overlong;
            continue;
        }
        m_buffer.append(c);
    }
    return lines;
}

} // namespace murasaki::screenmatch
