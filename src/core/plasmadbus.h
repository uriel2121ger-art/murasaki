#pragma once

#include "plasmacontrol.h"

namespace murasaki::plasma {

// The real backend: plasmashell's scripting interface and the daemon, both on the session bus. Blocking calls
// with a timeout; call it from the CLI or from a worker, never from a UI thread that must stay responsive.
class DBusBackend : public Backend
{
public:
    bool evaluate(const QString &script, QString *output, QString *error) override;
    bool presenter(QString *value, QString *error) override;
    bool setPresenter(const QString &presenter, QString *error) override;
};

} // namespace murasaki::plasma
