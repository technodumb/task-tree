#pragma once
// Local-LLM classifier backed by an Ollama-compatible HTTP endpoint. Built into
// the binary but only instantiated when [llm] enabled = true in the config.
// Every failure path (network, timeout, bad JSON, low confidence) degrades to a
// standalone result — it never blocks or throws into the UI.

#include <string>

#include "llm/IClassifier.hpp"

namespace tt {

class OllamaClassifier final : public IClassifier {
public:
    OllamaClassifier(std::string endpoint, std::string model,
                     float confidenceThreshold, int timeoutMs);

    bool enabled() const override { return true; }
    void classify(std::string newText, std::string existingTree,
                  ClassifyCallback done) override;

private:
    std::string endpoint_;
    std::string model_;
    float       confThreshold_;
    int         timeoutMs_;
};

} // namespace tt
