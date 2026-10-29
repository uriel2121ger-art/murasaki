#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace murasaki {

// The editable Wallpaper Engine user properties of one item, for the item page.
//
// Rows are the editable properties (slider, bool, color, combo, textinput, file, directory) ordered by `order`,
// then by their position in the item JSON; labels and groups are not rows. The effective value of a row is
// resolved as default -> preset -> assignment override -> edit.
//
// Conditions ("a.value && !b.value") are JavaScript expressions over `{value}` objects, one per property key.
// They come from untrusted project files, so they pass a token filter and run in a QJSEngine whose globals
// are stripped down to a few harmless ones. An expression that is rejected or fails keeps its row visible.
class PropertyModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        TypeRole,
        TextRole,
        ValueRole,
        MinRole,
        MaxRole,
        StepRole,
        EffectiveStepRole,
        OptionsRole,
        VisibleRole,
    };
    Q_ENUM(Role)

    explicit PropertyModel(QObject *parent = nullptr);
    ~PropertyModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // `item` is the daemon's full item JSON (properties, presetValues); `assignmentOverrides` is the
    // assignment's properties object. Resets the model and drops earlier edits.
    Q_INVOKABLE void load(const QVariantMap &item, const QVariantMap &assignmentOverrides);
    void load(const QJsonObject &item, const QJsonObject &assignmentOverrides);

    Q_INVOKABLE void setValue(int row, const QVariant &value);
    // Keys whose effective value differs from default+preset, with that effective value.
    Q_INVOKABLE QVariantMap overrides() const;
    Q_INVOKABLE void resetToDefaults();

    // Locale used to pick localized labels, as QLocale::name() ("es_MX"). Defaults to the system locale; tests inject one.
    void setLocale(const QString &localeName);

    // The label for a property `text` or option label: a key found in the item's `localization`
    // ({"en-us": {key: text}}) is translated (exact locale, same language, English, any), a `ui_` key is looked up in a
    // built-in table of common Wallpaper Engine keys, and anything left is humanized. Plain text passes through.
    static QString resolveLabel(const QString &raw, const QVariantMap &localization, const QString &localeName,
                                const QString &fallbackKey = {});

    // Plain text of a property label: tags removed, entities decoded, whitespace collapsed.
    static QString plainText(const QString &html);
    // Slider step the UI should use: the item's step, else 1 for integer ranges, else (max-min)/100.
    static double effectiveStep(double min, double max, double step);

signals:
    void countChanged();
    // The user changed a value or reset; the page persists overrides() in response.
    void edited();

protected:
    // Whether a condition (after `$` stripping) may reach the JS engine. Tests override it to exercise the
    // engine's own hardening without the token filter.
    virtual bool conditionAllowed(const QString &expr, const QSet<QString> &keys) const;

private:
    struct Row
    {
        QString key;
        QString type;
        QString text;
        QVariant base; // default + preset
        QVariant current; // effective value
        double min = 0;
        double max = 0;
        double step = 0;
        QVariantList options;
        QString condition;
        bool visible = true;
    };
    struct ScopeEntry
    {
        QString key;
        QVariant value;
    };

    void rebuild(const QVariantMap &item, const QVariantMap &assignmentOverrides);
    QVariant normalized(const QString &type, const QVariant &value, const Row &row) const;
    bool sameValue(const Row &row, const QVariant &a, const QVariant &b) const;
    // Recomputes `visible` of every row; returns the rows that changed.
    QList<int> updateVisibility();
    bool evaluate(const QString &condition, const QJSValueList &args);
    QJSValue compile(const QString &condition);
    void ensureEngine();

    QString m_locale;
    QList<Row> m_rows;
    QList<ScopeEntry> m_scope; // every identifier-safe property key, editable or not
    QVariantMap m_overrides;
    std::unique_ptr<QJSEngine> m_engine;
    QJSValue m_freeze;
    QHash<QString, QJSValue> m_compiled; // an undefined value marks an expression that cannot be used
    QSet<QString> m_warned;
};

} // namespace murasaki
