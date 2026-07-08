#include "llm/OllamaClassifier.hpp"

#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace tt {

using json = nlohmann::json;

OllamaClassifier::OllamaClassifier(std::string endpoint, std::string model,
                                   float confidenceThreshold, int timeoutMs)
    : endpoint_(std::move(endpoint)),
      model_(std::move(model)),
      confThreshold_(confidenceThreshold),
      timeoutMs_(timeoutMs) {}

namespace {

Relation relationFromString(const std::string& s) {
    if (s == "child_of")  return Relation::ChildOf;
    if (s == "parent_of") return Relation::ParentOf;
    return Relation::Standalone;
}

// Runs on a worker thread. Any failure returns a Standalone result.
ClassifyResult run(const std::string& endpoint, const std::string& model,
                   float confThreshold, int timeoutMs,
                   const std::string& newText, const std::string& tree) {
    ClassifyResult fallback; // Standalone
    try {
        const std::string treeStr = tree.empty() ? "(empty)\n" : tree;

        const std::string sys =
            "You organise tasks into a tree. The CURRENT tasks are listed one per line as "
            "'[id] parent=<pid|none>: text', where parent is that task's parent id (none = "
            "top-level). Reconstruct the hierarchy from those parent ids. "
            "Decide where a NEW task belongs and reply with ONLY JSON: "
            "{\"relation\":\"standalone|child_of|parent_of\",\"targetId\":<id or null>,"
            "\"confidence\":<0..1>}. child_of = the new task is a subtask of targetId. "
            "parent_of = the new task becomes the PARENT of targetId: it takes targetId's "
            "current position and targetId becomes its subtask (use this to insert BETWEEN "
            "targetId and its current parent). standalone = a new top-level task. Use the "
            "surrounding context (a task's parent and siblings) to place it precisely.";
        const std::string user =
            "CURRENT tasks:\n" + treeStr + "\nNEW task:\n" + newText;

        json body = {
            {"model", model},
            {"stream", false},
            {"format", "json"},
            {"options", {{"temperature", 0.1}}},
            {"messages", json::array({
                {{"role", "system"}, {"content", sys}},
                {{"role", "user"}, {"content", user}},
            })},
        };

        httplib::Client cli(endpoint);
        cli.set_connection_timeout(0, timeoutMs * 1000);
        cli.set_read_timeout(0, timeoutMs * 1000);
        cli.set_write_timeout(0, timeoutMs * 1000);

        auto res = cli.Post("/api/chat", body.dump(), "application/json");
        if (!res || res->status != 200) return fallback;

        json reply = json::parse(res->body, nullptr, false);
        if (reply.is_discarded()) return fallback;
        std::string content;
        if (reply.contains("message") && reply["message"].is_object())
            content = reply["message"].value("content", std::string{});
        if (content.empty()) return fallback;

        json parsed = json::parse(content, nullptr, false);
        if (parsed.is_discarded()) return fallback;

        ClassifyResult r;
        r.relation = relationFromString(parsed.value("relation", std::string{"standalone"}));
        if (parsed.contains("targetId") && parsed["targetId"].is_number_unsigned())
            r.targetId = parsed["targetId"].get<TaskId>();
        r.confidence = parsed.value("confidence", 0.f);

        if (r.relation == Relation::Standalone || r.targetId == 0 || r.confidence < confThreshold)
            return fallback;
        return r;
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace

void OllamaClassifier::classify(std::string newText, std::string existingTree,
                                ClassifyCallback done) {
    // Detached worker; the classifier outlives all requests (owned for the app's
    // lifetime), and the process exiting mid-request is harmless.
    std::thread([=, endpoint = endpoint_, model = model_,
                 conf = confThreshold_, timeout = timeoutMs_]() {
        ClassifyResult r = run(endpoint, model, conf, timeout, newText, existingTree);
        if (done) done(r);
    }).detach();
}

} // namespace tt
