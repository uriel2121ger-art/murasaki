#pragma once

#include <QString>
#include <QStringList>

namespace murasaki::import {

// The arguments of murasaki-sandbox for one import job, up to and not including the "--" and the program: the `import` profile (src/sandbox/profile.cpp,
// the one reviewed place that says what the helper may see) with the job's sources and its output root. The daemon passes only these two. `require` makes the
// sandbox refuse to run the program when Landlock is not available.
QStringList importSandboxArgs(const QStringList &sourcePaths, const QString &outRoot, bool require);

} // namespace murasaki::import
