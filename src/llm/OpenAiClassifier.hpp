#pragma once
// Classifier for any OpenAI-compatible chat-completions endpoint (Groq, OpenAI,
// local OpenAI-compatible servers, ...). Bearer-auth; talks to <endpoint>/chat/
// completions. Same graceful-degradation contract as the other classifiers: every
// failure path returns a standalone result and never blocks or throws into the UI.

#include <string>

#include "llm/IClassifier.hpp"

namespace tt {

class OpenAiClassifier final : public IClassifier {
public:
    // `endpoint` is the API base including any version path, e.g.
    // "https://api.groq.com/openai/v1". `apiKey` goes in the Authorization header.
    OpenAiClassifier(std::string endpoint, std::string apiKey, std::string model,
                     float confidenceThreshold, int timeoutMs);

    bool enabled() const override { return true; }
    void classify(std::string newText, std::string existingTree,
                  ClassifyCallback done) override;

private:
    std::string endpoint_;
    std::string apiKey_;
    std::string model_;
    float       confThreshold_;
    int         timeoutMs_;
};

} // namespace tt
