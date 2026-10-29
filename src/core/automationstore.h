#pragma once

// The three stores of the automation ($XDG_CONFIG_HOME/murasaki/profiles.json, triggers.json, timelines.json): atomic, validated, quarantined like pets.json.
// The list does the work once; each store is a thin QObject around it that signals changed().

#include "automation.h"
#include "jsonfile.h"
#include "profile.h"
#include "timeline.h"
#include "trigger.h"

#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QUuid>

#include <optional>

namespace murasaki {

struct ProfileTraits {
    using Item = Profile;
    static QLatin1String key() { return QLatin1String("profiles"); }
    static int max() { return kMaxProfiles; }
    static bool uniqueNames() { return true; }
    static std::optional<Item> parse(const QJsonObject &entry, QString *error)
    {
        Item base;
        base.id = entry.value(QLatin1String("id")).toString();
        return applyProfileChanges(base, entry, error);
    }
    static bool validate(const Item &item, QString *error) { return validateProfile(item, error); }
};

struct TriggerTraits {
    using Item = Trigger;
    static QLatin1String key() { return QLatin1String("triggers"); }
    static int max() { return kMaxTriggers; }
    static bool uniqueNames() { return false; }
    static std::optional<Item> parse(const QJsonObject &entry, QString *error)
    {
        Item base;
        base.id = entry.value(QLatin1String("id")).toString();
        return applyTriggerChanges(base, entry, error);
    }
    static bool validate(const Item &item, QString *error) { return validateTrigger(item, error); }
};

struct TimelineTraits {
    using Item = Timeline;
    static QLatin1String key() { return QLatin1String("timelines"); }
    static int max() { return kMaxTimelines; }
    static bool uniqueNames() { return true; }
    static std::optional<Item> parse(const QJsonObject &entry, QString *error)
    {
        Item base;
        base.id = entry.value(QLatin1String("id")).toString();
        return applyTimelineChanges(base, entry, error);
    }
    static bool validate(const Item &item, QString *error) { return validateTimeline(item, error); }
};

template <class Traits>
class AutomationList
{
public:
    using Item = typename Traits::Item;

    explicit AutomationList(QString path)
        : m_path(std::move(path))
    {
    }

    QString filePath() const { return m_path; }
    // A missing file means nothing; an unreadable or invalid file (or any invalid entry) is renamed to <file>.bad-<UTC time> and the list starts empty; if that rename
    // fails, saving stays disabled so the user's file is never overwritten.
    void load()
    {
        m_items.clear();
        m_saveBlocked = false;
        const JsonFileRead read = readJsonObjectFile(m_path);
        if (!read.exists)
            return;
        QString problem = read.problem;
        if (problem.isEmpty()) {
            const QString key = Traits::key();
            if (read.root.value(QLatin1String("version")).toInt() != 1 || !read.root.value(key).isArray()) {
                problem = QStringLiteral("unsupported format");
            } else {
                const QJsonArray entries = read.root.value(key).toArray();
                if (entries.size() > Traits::max())
                    problem = QStringLiteral("too many entries");
                for (const QJsonValue &value : entries) {
                    if (!problem.isEmpty())
                        break;
                    QString error;
                    const auto parsed = value.isObject() ? Traits::parse(value.toObject(), &error) : std::nullopt;
                    if (!parsed) {
                        problem = QStringLiteral("invalid entry: ") + (error.isEmpty() ? QStringLiteral("not an object") : error);
                    } else if (QUuid::fromString(parsed->id).isNull()) {
                        problem = QStringLiteral("invalid entry: id must be a uuid");
                    } else if (find(parsed->id) || nameTaken(*parsed)) {
                        problem = QStringLiteral("invalid entry: duplicate id or name");
                    } else {
                        m_items.append(*parsed);
                    }
                }
            }
        }
        if (problem.isEmpty())
            return;
        m_items.clear();
        const QString noun = Traits::key();
        if (!quarantineBadFile(m_path, problem, QStringLiteral("no ") + noun, noun))
            m_saveBlocked = true;
    }

    const QList<Item> &items() const { return m_items; }
    std::optional<Item> find(const QString &id) const
    {
        for (const Item &item : m_items)
            if (item.id == id)
                return item;
        return std::nullopt;
    }
    std::optional<Item> findByName(const QString &name) const
    {
        for (const Item &item : m_items)
            if (item.name.compare(name, Qt::CaseInsensitive) == 0)
                return item;
        return std::nullopt;
    }
    bool blocked() const { return m_saveBlocked; }

    // Validates, assigns a new uuid and saves: the id, or an empty string with *error set.
    QString add(Item item, QString *error = nullptr)
    {
        if (m_items.size() >= Traits::max()) {
            setError(error, QStringLiteral("at most %1 %2").arg(Traits::max()).arg(Traits::key()));
            return {};
        }
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!Traits::validate(item, error))
            return {};
        if (nameTaken(item)) {
            setError(error, QStringLiteral("the name is already used"));
            return {};
        }
        m_items.append(item);
        if (!save()) {
            m_items.removeLast();
            setError(error, QStringLiteral("could not save the %1").arg(Traits::key()));
            return {};
        }
        return item.id;
    }
    // Replaces the item with the same id. False without error when unknown or unchanged; false with *error when invalid or the write failed (nothing changed).
    bool update(const Item &item, QString *error = nullptr)
    {
        if (error)
            error->clear();
        if (!Traits::validate(item, error))
            return false;
        for (Item &existing : m_items) {
            if (existing.id != item.id)
                continue;
            if (existing == item)
                return false;
            if (nameTaken(item)) {
                setError(error, QStringLiteral("the name is already used"));
                return false;
            }
            const Item previous = existing;
            existing = item;
            if (!save()) {
                existing = previous;
                setError(error, QStringLiteral("could not save the %1").arg(Traits::key()));
                return false;
            }
            return true;
        }
        return false;
    }
    bool remove(const QString &id, QString *error = nullptr)
    {
        if (error)
            error->clear();
        for (qsizetype i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i).id != id)
                continue;
            const Item removed = m_items.takeAt(i);
            if (!save()) {
                m_items.insert(i, removed);
                setError(error, QStringLiteral("could not save the %1").arg(Traits::key()));
                return false;
            }
            return true;
        }
        return false;
    }

private:
    static void setError(QString *error, const QString &message)
    {
        if (error)
            *error = message;
    }
    // Another item (not the one with this id) already has the name.
    bool nameTaken(const Item &item) const
    {
        if (!Traits::uniqueNames())
            return false;
        for (const Item &other : m_items)
            if (other.id != item.id && other.name.compare(item.name, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    }
    bool save() const
    {
        if (m_saveBlocked) {
            qWarning("murasakid: not saving %s over the unreadable %s", qPrintable(Traits::key()), qPrintable(m_path));
            return false;
        }
        QJsonArray array;
        for (const Item &item : m_items)
            array.append(item.toJson());
        return writeJsonObjectFile(m_path, QJsonObject{{QStringLiteral("version"), 1}, {Traits::key(), array}});
    }

    QString m_path;
    QList<Item> m_items;
    bool m_saveBlocked = false;
};

class ProfileStore : public QObject
{
    Q_OBJECT

public:
    explicit ProfileStore(QString filePath, QObject *parent = nullptr)
        : QObject(parent)
        , m_list(std::move(filePath))
    {
    }
    QString filePath() const { return m_list.filePath(); }
    void load() { m_list.load(); }
    QList<Profile> profiles() const { return m_list.items(); }
    std::optional<Profile> profile(const QString &id) const { return m_list.find(id); }
    std::optional<Profile> profileByName(const QString &name) const { return m_list.findByName(name); }
    QString add(const Profile &profile, QString *error = nullptr)
    {
        const QString id = m_list.add(profile, error);
        if (!id.isEmpty())
            emit changed();
        return id;
    }
    bool update(const Profile &profile, QString *error = nullptr)
    {
        const bool done = m_list.update(profile, error);
        if (done)
            emit changed();
        return done;
    }
    bool remove(const QString &id, QString *error = nullptr)
    {
        const bool done = m_list.remove(id, error);
        if (done)
            emit changed();
        return done;
    }

signals:
    void changed();

private:
    AutomationList<ProfileTraits> m_list;
};

class TriggerStore : public QObject
{
    Q_OBJECT

public:
    explicit TriggerStore(QString filePath, QObject *parent = nullptr)
        : QObject(parent)
        , m_list(std::move(filePath))
    {
    }
    QString filePath() const { return m_list.filePath(); }
    void load() { m_list.load(); }
    QList<Trigger> triggers() const { return m_list.items(); }
    std::optional<Trigger> trigger(const QString &id) const { return m_list.find(id); }
    QString add(const Trigger &trigger, QString *error = nullptr)
    {
        const QString id = m_list.add(trigger, error);
        if (!id.isEmpty())
            emit changed();
        return id;
    }
    bool update(const Trigger &trigger, QString *error = nullptr)
    {
        const bool done = m_list.update(trigger, error);
        if (done)
            emit changed();
        return done;
    }
    bool remove(const QString &id, QString *error = nullptr)
    {
        const bool done = m_list.remove(id, error);
        if (done)
            emit changed();
        return done;
    }

signals:
    void changed();

private:
    AutomationList<TriggerTraits> m_list;
};

class TimelineStore : public QObject
{
    Q_OBJECT

public:
    explicit TimelineStore(QString filePath, QObject *parent = nullptr)
        : QObject(parent)
        , m_list(std::move(filePath))
    {
    }
    QString filePath() const { return m_list.filePath(); }
    void load() { m_list.load(); }
    QList<Timeline> timelines() const { return m_list.items(); }
    std::optional<Timeline> timeline(const QString &id) const { return m_list.find(id); }
    std::optional<Timeline> timelineByName(const QString &name) const { return m_list.findByName(name); }
    QString add(const Timeline &timeline, QString *error = nullptr)
    {
        const QString id = m_list.add(timeline, error);
        if (!id.isEmpty())
            emit changed();
        return id;
    }
    bool update(const Timeline &timeline, QString *error = nullptr)
    {
        const bool done = m_list.update(timeline, error);
        if (done)
            emit changed();
        return done;
    }
    bool remove(const QString &id, QString *error = nullptr)
    {
        const bool done = m_list.remove(id, error);
        if (done)
            emit changed();
        return done;
    }

signals:
    void changed();

private:
    AutomationList<TimelineTraits> m_list;
};

} // namespace murasaki
