#pragma once

#include <QString>

namespace murasaki::import {

// Removes the <id>.part folders a crashed job left in outRoot (call it when no job runs). Returns how many.
int cleanStaleImportParts(const QString &outRoot);

} // namespace murasaki::import
