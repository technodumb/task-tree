#pragma once
// Default classifier: everything is standalone. Used when no local LLM is
// configured (the shipped default).

#include "llm/IClassifier.hpp"

namespace tt {

struct NullClassifier final : IClassifier {
    bool enabled() const override { return false; }

    void classify(std::string, std::string, ClassifyCallback done) override {
        if (done) done(ClassifyResult{}); // Relation::Standalone
    }
};

} // namespace tt
