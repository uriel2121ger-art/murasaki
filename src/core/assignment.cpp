#include "assignment.h"

#include "jsonfile.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr qsizetype kMaxKeyLength = 200;
// Far below the file limit of readJsonObjectFile, so a store full of valid assignments always stays loadable.
constexpr qsizetype kMaxPropertiesSize = 1024 * 1024;
static_assert(kMaxPropertiesSize * 2 < kMaxJsonFileSize);

bool isValidFill(const QString &fill)
{
    return fill == u"fill" || fill == u"fit" || fill == u"stretch";
}

// The one place that decides what a storable assignment is: makeAssignment and AssignmentStore::set() share it, so
// set() can never write a file that load() refuses.
bool validateAssignment(const Assignment &assignment, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!isValidKeyPart(assignment.output))
        return fail(u"output must be 1-200 characters"_s);
    if (!isValidKeyPart(assignment.desktop))
        return fail(u"desktop must be 1-200 characters"_s);
    if (!isValidKeyPart(assignment.itemId))
        return fail(u"itemId must be 1-200 characters"_s);
    if (!isValidFill(assignment.fill))
        return fail(u"fill must be fill, fit or stretch"_s);
    if (assignment.span && assignment.output != kWildcard)
        return fail(u"span is only valid with output *"_s);
    if (QJsonDocument(assignment.properties).toJson(QJsonDocument::Compact).size() > kMaxPropertiesSize)
        return fail(u"properties are larger than 1 MiB"_s);
    return true;
}

} // namespace

QJsonObject Assignment::toJson() const
{
    QJsonObject json{
        {u"output"_s, output},
        {u"desktop"_s, desktop},
        {u"itemId"_s, itemId},
        {u"muted"_s, muted},
        {u"fill"_s, fill},
        {u"properties"_s, properties},
    };
    if (span) // left out when off, so that every assignment made before spanning keeps the same JSON
        json.insert(u"span"_s, true);
    return json;
}

bool isValidKeyPart(const QString &part)
{
    return !part.isEmpty() && part.size() <= kMaxKeyLength;
}

std::optional<Assignment> makeAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                         const QJsonObject &options, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<Assignment> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    Assignment assignment;
    assignment.output = output;
    assignment.desktop = desktop;
    assignment.itemId = itemId;
    if (options.contains(u"muted"_s)) {
        const QJsonValue muted = options.value(u"muted"_s);
        if (!muted.isBool())
            return fail(u"muted must be a boolean"_s);
        assignment.muted = muted.toBool();
    }
    if (options.contains(u"fill"_s)) {
        const QString fill = options.value(u"fill"_s).toString();
        if (!isValidFill(fill))
            return fail(u"fill must be fill, fit or stretch"_s);
        assignment.fill = fill;
    }
    if (options.contains(u"span"_s)) {
        const QJsonValue span = options.value(u"span"_s);
        if (!span.isBool())
            return fail(u"span must be a boolean"_s);
        assignment.span = span.toBool();
    }
    if (options.contains(u"properties"_s)) {
        const QJsonValue properties = options.value(u"properties"_s);
        if (!properties.isObject())
            return fail(u"properties must be an object"_s);
        assignment.properties = properties.toObject();
    }
    QString problem;
    if (!validateAssignment(assignment, &problem))
        return fail(problem);
    return assignment;
}

AssignmentStore::AssignmentStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void AssignmentStore::load()
{
    m_entries.clear();
    m_saveBlocked = false;
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;

    QString problem = read.problem;
    if (problem.isEmpty()) {
        if (read.root.value(u"version"_s).toInt() != 1 || !read.root.value(u"assignments"_s).isArray()) {
            problem = u"unsupported format"_s;
        } else {
            for (const QJsonValue &value : read.root.value(u"assignments"_s).toArray()) {
                const QJsonObject entry = value.toObject();
                QString error;
                const auto assignment = makeAssignment(entry.value(u"output"_s).toString(),
                                                       entry.value(u"desktop"_s).toString(),
                                                       entry.value(u"itemId"_s).toString(), entry, &error);
                if (!assignment) {
                    problem = u"invalid entry: "_s + error;
                    break;
                }
                m_entries.insert({assignment->output, assignment->desktop}, *assignment);
            }
        }
    }
    if (problem.isEmpty())
        return;

    m_entries.clear();
    if (!quarantineBadFile(m_path, problem, u"no assignments"_s, u"assignments"_s))
        m_saveBlocked = true;
}

QList<Assignment> AssignmentStore::assignments() const
{
    return m_entries.values();
}

std::optional<Assignment> AssignmentStore::resolve(const QString &output, const QString &desktop) const
{
    const QString any = kWildcard;
    for (const Key &key : {Key{output, desktop}, Key{output, any}, Key{any, desktop}, Key{any, any}}) {
        const auto it = m_entries.constFind(key);
        if (it != m_entries.cend())
            return *it;
    }
    return std::nullopt;
}

bool AssignmentStore::referencesItem(const QString &itemId) const
{
    for (const Assignment &assignment : m_entries) {
        if (assignment.itemId == itemId)
            return true;
    }
    return false;
}

bool AssignmentStore::set(const Assignment &assignment)
{
    QString problem;
    if (!validateAssignment(assignment, &problem)) {
        qWarning("murasakid: rejecting invalid assignment for %s/%s: %s", qPrintable(assignment.output),
                 qPrintable(assignment.desktop), qPrintable(problem));
        return false;
    }
    const Key key{assignment.output, assignment.desktop};
    const auto it = m_entries.constFind(key);
    if (it != m_entries.cend() && *it == assignment)
        return false;
    m_entries.insert(key, assignment);
    save();
    emit changed();
    return true;
}

bool AssignmentStore::clear(const QString &output, const QString &desktop)
{
    if (m_entries.remove({output, desktop}) == 0)
        return false;
    save();
    emit changed();
    return true;
}

void AssignmentStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving assignments over the unreadable %s", qPrintable(m_path));
        return;
    }
    QJsonArray array;
    for (const Assignment &assignment : m_entries)
        array.append(assignment.toJson());
    writeJsonObjectFile(m_path, QJsonObject{{u"version"_s, 1}, {u"assignments"_s, array}});
}

} // namespace murasaki
