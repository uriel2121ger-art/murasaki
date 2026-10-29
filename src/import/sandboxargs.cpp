#include "sandboxargs.h"

#include <QFileInfo>

namespace murasaki::import {

QStringList importSandboxArgs(const QStringList &sourcePaths, const QString &outRoot, bool require)
{
    QStringList args{QStringLiteral("--profile"), QStringLiteral("import")};
    for (const QString &source : sourcePaths) {
        if (QFileInfo::exists(source)) // a rule needs a path that exists; a missing source is the helper's "source not found"
            args << QStringLiteral("--src") << source;
    }
    args << QStringLiteral("--out") << outRoot;
    if (require)
        args << QStringLiteral("--require");
    return args;
}

} // namespace murasaki::import
