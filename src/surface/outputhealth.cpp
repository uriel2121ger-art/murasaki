#include "outputhealth.h"

#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

QString failureKindName(FailureKind kind)
{
    switch (kind) {
    case FailureKind::Refused:
        return u"refused"_s;
    case FailureKind::Crashed:
        return u"crashed"_s;
    case FailureKind::NoFrame:
        return u"noframe"_s;
    }
    return u"refused"_s;
}

FailureKind classifyFailure(const QString &reason)
{
    if (reason.contains(u"first frame"_s, Qt::CaseInsensitive))
        return FailureKind::NoFrame;
    for (const QString &word : {u"exited"_s, u"crash"_s, u"killed"_s, u"signal"_s, u"context unavailable"_s}) {
        if (reason.contains(word, Qt::CaseInsensitive))
            return FailureKind::Crashed;
    }
    return FailureKind::Refused;
}

bool OutputHealth::begin(const QString &key)
{
    if (key == m_key)
        return false; // the same content again (a setting that did not change it): nothing new
    m_key = key;
    return std::exchange(m_failure, std::nullopt).has_value();
}

bool OutputHealth::report(const QString &key, const OutputFailure &failure)
{
    if (key != m_key || m_failure == failure)
        return false;
    m_failure = failure;
    return true;
}

bool OutputHealth::frame(const QString &key)
{
    if (key != m_key || !m_failure || m_failure->kind != FailureKind::NoFrame)
        return false;
    m_failure.reset();
    return true;
}

bool OutputHealth::retry()
{
    return std::exchange(m_failure, std::nullopt).has_value();
}

} // namespace murasaki
