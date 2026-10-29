#pragma once

// How the controller gets the hit masks of a pet that plays through the video path (no pixels in the GUI): a background job that decodes the
// file it plays once, keeps only the masks and caches them on disk (loadMaskPass in spritemask.h). An interface so that the controller, which
// has no decoder, can be tested with a fake that answers when the test says so.

#include "alphamask.h"

#include <QString>

#include <functional>
#include <memory>

namespace murasaki {

class MaskLoader
{
public:
    // Called on the controller's thread; the set is null when the pass failed. `file` says which request it answers.
    using Done = std::function<void(const QString &file, std::shared_ptr<const MaskSet> masks)>;

    virtual ~MaskLoader() = default;
    // Starts the pass for `file` (the one the pet plays). `key` identifies the pet: a newer load() for the same key cancels the older.
    virtual void load(const QString &key, const QString &item, const QString &variant, const QString &file, std::size_t budgetBytes, Done done) = 0;
    // The pet is gone or plays another file: its job, queued or running, is dropped and its answer never delivered.
    virtual void cancel(const QString &key) = 0;
};

} // namespace murasaki
