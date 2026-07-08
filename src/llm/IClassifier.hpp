#pragma once
// Pluggable task classifier seam. When enabled, decides whether a newly created
// task should relate to an existing one. Runs OFF the UI thread and never blocks
// task creation — the task is created standalone immediately and (optionally)
// reparented later when a result arrives.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "model/Task.hpp"

namespace tt {

enum class Relation { Standalone, ChildOf, ParentOf };

struct ClassifyResult {
    Relation relation = Relation::Standalone;
    TaskId   targetId = 0;      // the existing task to relate to (0 => none)
    float    confidence = 0.f;  // 0..1
};

// Invoked when classification completes. MAY be called on a worker thread, so the
// implementation of the callback must marshal back to the main thread itself.
using ClassifyCallback = std::function<void(ClassifyResult)>;

struct IClassifier {
    virtual ~IClassifier() = default;

    // Whether classification is active. When false the app skips classify() and
    // every task stays standalone.
    virtual bool enabled() const = 0;

    // Classify `newText` against `existingTree` — the current (non-DONE) task tree as
    // an indented outline where indentation denotes subtasks and each line is
    // "[id] text". Passing the structure (not just a flat list) lets the model place
    // the task precisely and use parent/sibling context. Calls `done` exactly once;
    // must not throw into the caller and must do the work asynchronously.
    virtual void classify(std::string newText, std::string existingTree,
                          ClassifyCallback done) = 0;
};

} // namespace tt
