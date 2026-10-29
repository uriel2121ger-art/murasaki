#include "murasakiservice.h"
#include "murasakiserviceinternal.h"

#include "automationmanager.h"

#include "localpets.h"
#include "localpuppets.h"
#include "petaudiomanager.h"
#include "puppetchecker.h"
#include "importablefiles.h"

#include "constants.h"
#include "playlistmanager.h"
#include "shortcut/launchassigned.h"
#include "shortcut/shortcutpolicy.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

QString MurasakiService::addLocalPet(const QString &path, QString *problem)
{
    const QString canonical = LocalPetImporter::validate(path, problem);
    if (canonical.isEmpty())
        return {};
    if (!m_localPets->add(canonical, problem) && !problem->isEmpty())
        return {};
    m_library->setLocalPetFiles(m_localPets->files());
    m_library->rescan();
    return LocalPetImporter::itemIdFor(canonical);
}

QString MurasakiService::AddLocalPetItem(const QString &path)
{
    if (!m_localPets) {
        rejectCall(u"local pets are not available"_s);
        return {};
    }
    QString problem;
    const QString id = addLocalPet(path, &problem);
    if (id.isEmpty()) {
        rejectCall(problem);
        return id;
    }
    // The user's setting: a GIF that is added is also converted to APNG (the GIF stays, as the pet already added).
    if (m_imports && m_settings->importSettings().convertGifOnImport && path.endsWith(u".gif"_s, Qt::CaseInsensitive)) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        QString ignored;
        const QJsonObject spec{{u"kind"_s, u"gif"_s}, {u"name"_s, QFileInfo(path).completeBaseName()}, {u"sources"_s, QJsonArray{canonical}}};
        m_imports->submit(QJsonDocument(spec).toJson(QJsonDocument::Compact), &ignored);
    }
    return id;
}

bool MurasakiService::isGenerated(const WallpaperItem &item) const
{
    if (!m_imports || item.source != u"local"_s)
        return false;
    const QString root = QFileInfo(m_imports->outRoot()).canonicalFilePath();
    return !root.isEmpty() && QDir::cleanPath(item.dir).startsWith(root + u'/');
}

QJsonObject MurasakiService::packageRecord(const WallpaperItem &item) const
{
    return readRecordFile(item.dir + u"/source.json"_s).value(u"mdaa"_s).toObject();
}

void MurasakiService::attachImports(PetImportManager *imports)
{
    m_imports = imports;
    m_autoImportedFile = QDir::cleanPath(imports->outRoot() + u"/../auto-imported.json"_s);
    QFile remembered(m_autoImportedFile);
    if (remembered.open(QIODevice::ReadOnly) && remembered.size() < (4 << 20)) {
        for (const QJsonValue &v : QJsonDocument::fromJson(remembered.readAll()).array())
            m_autoImported.insert(v.toString());
    }
    connect(m_library, &Library::itemsChanged, this, [this] { autoImportWorkshopFiles(); });
    connect(m_settings, &SettingsStore::changed, this, [this] {
        const ImportSettings &prefs = m_settings->importSettings();
        const QString now = prefs.workshopFiles + u'/' + prefs.workshopMdaa;
        if (now == m_lastAutoSettings) // some other setting changed: nothing to look at again
            return;
        m_lastAutoSettings = now;
        autoImportWorkshopFiles();
    });
    QTimer::singleShot(0, this, [this] { autoImportWorkshopFiles(); });
    connect(m_imports, &PetImportManager::changed, this, &MurasakiService::ImportChanged);
    connect(m_imports, &PetImportManager::done, this, [this](const QString &jobId, const QString &file) {
        QString problem;
        // The helper's word is checked: what it names must be inside the import root (a path from a misbehaving helper is not a pet).
        const QString root = QFileInfo(m_imports->outRoot()).canonicalFilePath();
        const QString real = QFileInfo(file).canonicalFilePath();
        QString itemId;
        if (!m_localPets)
            problem = u"local pets are not available"_s;
        else if (root.isEmpty() || real.isEmpty() || !real.startsWith(root + u'/'))
            problem = u"the helper named a file outside the import folder"_s;
        else
            itemId = addLocalPet(real, &problem);
        if (itemId.isEmpty()) {
            qWarning().noquote() << "murasakid: the imported pet cannot be added:" << problem;
            m_imports->markFailed(jobId, u"helper failed: the result cannot be added as a pet: "_s + problem.left(200));
            return;
        }
        m_imports->setItemId(jobId, itemId);
    });
    // A queue that was full lets go of files when a job ends: look again then.
    connect(m_imports, &PetImportManager::changed, this, [this](const QString &jobId) {
        const QJsonObject job = m_imports->job(jobId);
        const QString state = job.value(u"state"_s).toString();
        if (state == u"done"_s || state == u"failed"_s || state == u"cancelled"_s)
            QTimer::singleShot(0, this, [this] { autoImportWorkshopFiles(); });
    });
}

QString MurasakiService::ImportPet(const QString &specJson)
{
    if (!m_imports || !m_localPets) {
        rejectCall(u"imports are not available"_s);
        return {};
    }
    QString problem;
    const QString id = m_imports->submit(specJson.toUtf8(), &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

QString MurasakiService::PreviewImport(const QString &specJson)
{
    if (!m_imports) {
        rejectCall(u"imports are not available"_s);
        return {};
    }
    QString problem;
    if (!calledFromDBus()) { // (in-process callers, the tests) wait for the helper
        const QString result = m_imports->preview(specJson.toUtf8(), &problem);
        if (result.isEmpty())
            rejectCall(problem);
        return result;
    }
    // Over D-Bus the reply is sent when the helper has answered: the daemon keeps running meanwhile, one preview at a time, repeats share the answer.
    const QDBusMessage request = message();
    const QDBusConnection bus = connection();
    setDelayedReply(true);
    const bool queued = m_imports->previewAsync(
        specJson.toUtf8(), [request, bus](const QString &answer) { QDBusConnection(bus).send(request.createReply(answer)); }, &problem);
    if (!queued)
        QDBusConnection(bus).send(request.createErrorReply(QDBusError::InvalidArgs, problem));
    return {};
}

QString MurasakiService::ImportState() const
{
    return QString::fromUtf8(QJsonDocument(m_imports ? m_imports->jobs() : QJsonArray()).toJson(QJsonDocument::Compact));
}

void MurasakiService::CancelImport(const QString &jobId)
{
    if (m_imports && !m_imports->cancel(jobId))
        rejectCall(u"No such import to cancel: "_s + jobId.left(100));
}

void MurasakiService::RemoveImportedPet(const QString &itemId, bool deleteFiles)
{
    if (!m_localPets) {
        rejectCall(u"local pets are not available"_s);
        return;
    }
    if (m_pets && m_pets->referencesItem(itemId)) {
        rejectCall(u"Remove the pets that show this item first: "_s + itemId.left(200));
        return;
    }
    QString problem;
    const std::optional<QString> path = m_localPets->remove(itemId, &problem);
    if (!path) {
        rejectCall(problem.isEmpty() ? u"Unknown local pet item: "_s + itemId.left(200) : problem);
        return;
    }
    if (deleteFiles && m_imports) {
        // Only the job folder directly under the import root, and only for a file that is inside it.
        const QString root = QFileInfo(m_imports->outRoot()).canonicalFilePath();
        const QFileInfo file(*path);
        const QDir folder = file.isDir() ? QDir(file.absoluteFilePath()) : file.absoluteDir(); // a package IS its folder
        if (!root.isEmpty() && folder.absolutePath().startsWith(root + u'/') && QFileInfo(folder.absolutePath()).dir().absolutePath() == root
            && QFileInfo::exists(folder.filePath(u"source.json"_s)))
            QDir(folder.absolutePath()).removeRecursively();
    }
    m_library->setLocalPetFiles(m_localPets->files());
    m_library->rescan();
}

void MurasakiService::RemoveLocalPetItem(const QString &itemId)
{
    if (!m_localPets) {
        rejectCall(u"local pets are not available"_s);
        return;
    }
    if (m_pets && m_pets->referencesItem(itemId)) {
        rejectCall(u"Remove the pets that show this item first: "_s + itemId.left(200));
        return;
    }
    QString problem;
    if (!m_localPets->remove(itemId, &problem)) {
        rejectCall(problem.isEmpty() ? u"Unknown local pet item: "_s + itemId.left(200) : problem);
        return;
    }
    m_library->setLocalPetFiles(m_localPets->files());
    m_library->rescan();
}

QString MurasakiService::AddLocalPuppetItem(const QString &path)
{
    if (!m_localPuppets) {
        rejectCall(u"puppets are not available"_s);
        return {};
    }
    QString problem;
    const QString canonical = LocalPuppetImporter::validate(path, &problem);
    if (canonical.isEmpty()) {
        rejectCall(problem);
        return {};
    }
    if (m_localPuppets->files().size() >= kMaxLocalPuppets && !m_localPuppets->files().contains(canonical)) {
        rejectCall(u"at most 64 puppets"_s);
        return {};
    }
    if (!m_localPuppets->add(canonical, &problem) && !problem.isEmpty()) {
        rejectCall(problem);
        return {};
    }
    m_library->setLocalPuppetFolders(m_localPuppets->files());
    m_library->rescan();
    return LocalPuppetImporter::itemIdFor(canonical);
}

void MurasakiService::RemoveLocalPuppetItem(const QString &itemId)
{
    if (!m_localPuppets) {
        rejectCall(u"puppets are not available"_s);
        return;
    }
    if (m_pets && m_pets->referencesItem(itemId)) {
        rejectCall(u"Remove the pets that show this item first: "_s + itemId.left(200));
        return;
    }
    QString problem;
    const std::optional<QString> folder = m_localPuppets->remove(itemId, &problem);
    if (!folder) {
        rejectCall(problem.isEmpty() ? u"Unknown puppet item: "_s + itemId.left(200) : problem);
        return;
    }
    if (m_puppetChecker)
        m_puppetChecker->forget(*folder);
    m_library->setLocalPuppetFolders(m_localPuppets->files());
    m_library->rescan();
}

} // namespace murasaki
