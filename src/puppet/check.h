#pragma once

#include "puppet/manifest.h"

#include <string>
#include <string_view>

namespace murasaki::puppet {

// `murasaki-puppet --check FOLDER`: the line it prints (one line, no newline at the end), docs/pets/puppets.md section 11. Keys, in this order:
//   ok, format ("cubism", "open" or null), modelVersion (the manifest's or container's version, or null), parameters, drawables (null: the manifest cannot tell), textures,
//   motions, expressions (counts, null unless ok), physics (null: counted when the physics file is read), canvas ([width, height] in model units of an Inochi2D puppet at rest, null when not known), reasons, degraded (lists of "<name>[: <detail>]"), engine, version.
// A Cubism item is `ok` with the degraded entry "puppet: needs the Cubism runtime": it can be drawn only through a Core library the user installs.
std::string checkJson(const ManifestResult &result, std::string_view version);

// The check of an item: loadManifest, then, for an Inochi2D container, the whole puppet is read and evaluated once at rest (OpenModel) so that a puppet that cannot be drawn is refused here and
// the counts are known. A failure of that read is reported as `puppet: unsupported format` (a container this build does not read), `puppet: too large` (a limit) or `puppet: manifest invalid` (anything else).
ManifestResult checkItem(const std::string &folder);

// The text a detail is cut to and cleaned to for the line (invalid UTF-8 becomes U+FFFD).
std::string tidyDetail(std::string_view detail);

} // namespace murasaki::puppet
