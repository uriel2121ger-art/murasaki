#include "puppetchecker.h"

#include "sceneenginelookup.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QJsonObject failure(const QString &why)
{
    return QJsonObject{{u"ok"_s, false}, {u"reasons"_s, QJsonArray{u"puppet: check failed: "_s + why.left(200)}}, {u"degraded"_s, QJsonArray()}};
}

} // namespace

PuppetChecker::PuppetChecker(QString helper, QString sandbox, QObject *parent)
    : QObject(parent)
    , m_helper(std::move(helper))
    , m_sandbox(std::move(sandbox))
{
}

PuppetChecker::~PuppetChecker()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString PuppetChecker::signatureOf(const QString &folder)
{
    // The top level of the folder is what tells that the puppet changed (a replaced .inp, an edited manifest); a file inside a sub-folder changing alone is not noticed until the next start.
    QByteArray text;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden, QDir::Name);
    for (const QFileInfo &e : entries.mid(0, 2000))
        text += e.fileName().toUtf8() + '|' + QByteArray::number(e.size()) + '|' + QByteArray::number(e.lastModified().toMSecsSinceEpoch()) + '\n';
    return QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha1).toHex());
}

QStringList PuppetChecker::command(const QString &helper, const QString &sandbox, bool require, const QString &folder)
{
    QStringList out;
    if (!sandbox.isEmpty()) {
        out << sandbox << u"--profile"_s << u"puppet"_s << u"--item"_s << folder;
        if (require)
            out << u"--require"_s;
        out << u"--"_s;
    }
    out << helper << u"--check"_s << folder;
    return out;
}

QJsonObject PuppetChecker::result(const QString &folder)
{
    const QString signature = signatureOf(folder);
    Entry &entry = m_entries[folder];
    if (entry.signature != signature) { // new, or the folder changed: the old answer is not about it any more
        entry.signature = signature;
        entry.answer = {};
    }
    if (entry.answer.isEmpty() && !entry.running && std::find(m_queue.begin(), m_queue.end(), folder) == m_queue.end() && int(m_queue.size()) < kMaxQueued) {
        m_queue.push_back(folder);
        QTimer::singleShot(0, this, [this] { startNext(); });
    }
    return entry.answer;
}

void PuppetChecker::forget(const QString &folder)
{
    m_entries.remove(folder);
    const auto it = std::find(m_queue.begin(), m_queue.end(), folder);
    if (it != m_queue.end())
        m_queue.erase(it);
}

void PuppetChecker::startNext()
{
    if (m_process || m_queue.empty())
        return;
    const QString folder = m_queue.front();
    m_queue.pop_front();
    Entry &entry = m_entries[folder];
    const Sandbox mode = m_mode ? m_mode() : Sandbox::Prefer;
    const QString helper = !m_helper.isEmpty() ? m_helper : findPuppetHelper();
    const QString sandbox = mode == Sandbox::Off ? QString() : (!m_sandbox.isEmpty() ? m_sandbox : findSandboxHelper());
    if (helper.isEmpty()) {
        finish(folder, entry.signature, failure(u"murasaki-puppet is not installed"_s));
        return startNext();
    }
    if (mode == Sandbox::Require && sandbox.isEmpty()) {
        finish(folder, entry.signature, failure(u"sandbox unavailable"_s));
        return startNext();
    }
    entry.running = true;
    m_current = folder;
    m_currentSignature = entry.signature;
    m_output.clear();
    m_timedOut = false;
    m_tooLong = false;
    QStringList program = command(helper, sandbox, mode == Sandbox::Require, folder);
    m_process = std::make_unique<QProcess>();
    // The helper reads hostile models: if one takes it over, what it writes must not grow this process. Standard output is drained as it arrives and kept up to a small bound; past it the helper is killed and
    // what still arrives is thrown away. Standard error goes nowhere.
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setStandardErrorFile(QProcess::nullDevice());
    QProcess *process = m_process.get();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (m_tooLong) {
            process->readAllStandardOutput(); // (thrown away: the kill is on its way)
            return;
        }
        m_output += process->read(kMaxOutputBytes + 1 - m_output.size()); // (never more than the bound plus one byte, to tell it is over)
        if (m_output.size() > kMaxOutputBytes) {
            m_tooLong = true;
            process->kill(); // (finished follows)
        }
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (process != m_process.get())
            return;
        if (!m_tooLong && m_output.size() <= kMaxOutputBytes)
            m_output += process->read(kMaxOutputBytes + 1 - m_output.size());
        if (m_output.size() > kMaxOutputBytes)
            m_tooLong = true;
        const int newline = m_output.indexOf('\n');
        const QJsonDocument doc = QJsonDocument::fromJson(m_output.left(newline >= 0 ? newline : m_output.size()));
        QJsonObject answer;
        if (m_timedOut)
            answer = failure(u"it took too long"_s);
        else if (m_tooLong)
            answer = failure(u"its output is too long"_s);
        else if (status != QProcess::NormalExit || code != 0 || !doc.isObject())
            answer = failure(status != QProcess::NormalExit ? u"the checker crashed"_s : u"the checker exited with status %1"_s.arg(code));
        else
            answer = doc.object();
        complete(answer);
    });
    // A process that does not start never finishes (Qt emits no `finished`): this is its end, or the queue would wait for ever.
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && process == m_process.get())
            complete(failure(u"the checker cannot be started"_s));
    });
    QTimer::singleShot(m_deadlineMs, process, [this, process] {
        if (process == m_process.get() && process->state() != QProcess::NotRunning) {
            m_timedOut = true;
            process->kill();
        }
    });
    const QString exe = program.takeFirst();
    m_process->start(exe, program);
}

// The end of the check that runs: the process goes, the answer is kept, the next check starts.
void PuppetChecker::complete(const QJsonObject &answer)
{
    const QString folder = m_current;
    const QString signature = m_currentSignature;
    m_current.clear();
    if (m_process) {
        m_process->disconnect(this);
        m_process.release()->deleteLater();
    }
    finish(folder, signature, answer);
    QTimer::singleShot(0, this, [this] { startNext(); });
}

void PuppetChecker::finish(const QString &folder, const QString &signature, const QJsonObject &answer)
{
    const auto it = m_entries.find(folder);
    if (it == m_entries.end())
        return; // forgotten meanwhile
    it->running = false;
    // An answer about a folder that changed while it was checked is not about the folder now: it is dropped, and the listing that follows asks again.
    if (it->signature == signature)
        it->answer = answer;
    emit changed(folder);
}

} // namespace murasaki
