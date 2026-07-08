#include "llm/OpenAiClassifier.hpp"

#include <chrono>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "llm/LlmLog.hpp"

namespace tt {

using json = nlohmann::json;

OpenAiClassifier::OpenAiClassifier(std::string endpoint, std::string apiKey, std::string model,
                                   float confidenceThreshold, int timeoutMs)
    : endpoint_(std::move(endpoint)),
      apiKey_(std::move(apiKey)),
      model_(std::move(model)),
      confThreshold_(confidenceThreshold),
      timeoutMs_(timeoutMs) {}

namespace {

Relation relationFromString(const std::string& s) {
    if (s == "child_of")  return Relation::ChildOf;
    if (s == "parent_of") return Relation::ParentOf;
    return Relation::Standalone;
}

// Extract the first {...} JSON object from a string (tolerates code fences / prose).
std::string extractJsonObject(const std::string& s) {
    const auto a = s.find('{');
    const auto b = s.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return {};
    return s.substr(a, b - a + 1);
}

ClassifyResult run(const std::string& endpoint, const std::string& apiKey,
                   const std::string& model, float confThreshold, int timeoutMs,
                   const std::string& newText, const std::string& tree) {
    ClassifyResult fallback; // Standalone
    const bool L = llmlog::enabled();
    std::string log;
    auto add = [&](const std::string& s) { if (L) log += s; };
    auto bail = [&](const std::string& reason) -> ClassifyResult {
        if (L) { log += "RESULT: standalone (" + reason + ")\n"; llmlog::write(log); }
        return fallback;
    };
    try {
        // Split "<scheme>://<host>[:port]/<path>" into origin + path prefix.
        std::string origin = endpoint, prefix;
        const auto schemeEnd = endpoint.find("://");
        const auto hostStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
        const auto slash = endpoint.find('/', hostStart);
        if (slash != std::string::npos) {
            origin = endpoint.substr(0, slash);
            prefix = endpoint.substr(slash); // e.g. "/v1"
        }

        const std::string treeStr = tree.empty() ? "(empty)\n" : tree;

        const std::string sys =
            "You organise tasks into a tree. The CURRENT tasks are listed one per line as "
            "'[id] parent=<pid|none>: text', where parent is that task's parent id (none = "
            "top-level). Reconstruct the hierarchy from those parent ids. "
            "Decide where a NEW task belongs and reply with ONLY JSON: "
            "{\"relation\":\"standalone|child_of|parent_of\",\"targetId\":<id or null>,"
            "\"confidence\":<0..1>}. child_of = the new task is a subtask of targetId. "
            "parent_of = the new task becomes the PARENT of targetId: it takes targetId's "
            "current position in the tree and targetId becomes its subtask — use this to "
            "insert the new task BETWEEN targetId and targetId's current parent. "
            "standalone = it starts a new top-level task. Use the surrounding context "
            "(a task's parent and siblings) to place it precisely.";
        const std::string user =
            "CURRENT tasks:\n" + treeStr + "\nNEW task:\n" + newText;

        add("provider: openai-compatible\nendpoint: " + endpoint + "\nmodel: " + model + "\n");
        add("NEW: " + newText + "\nTREE:\n" + treeStr);
        add("--- system ---\n" + sys + "\n--- user ---\n" + user + "\n");

        json body = {
            {"model", model},
            {"temperature", 0.1},
            {"messages", json::array({
                {{"role", "system"}, {"content", sys}},
                {{"role", "user"}, {"content", user}},
            })},
        };

        httplib::Client cli(origin);
        cli.set_connection_timeout(0, timeoutMs * 1000);
        cli.set_read_timeout(0, timeoutMs * 1000);
        cli.set_write_timeout(0, timeoutMs * 1000);
        // (TLS server-cert verification is on by default when built with OpenSSL.)

        httplib::Headers headers = {{"Authorization", "Bearer " + apiKey}};
        const std::string body_str = body.dump();
        const std::string route = prefix + "/chat/completions";

        // Retry transient transport failures (dropped connection, TLS blip, timeout,
        // flaky IPv6 — Cerebras is IPv6/Cloudflare). Do NOT retry real HTTP responses
        // (a 4xx won't fix itself); those are handled below.
        httplib::Result res;
        std::string lastErr;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            res = cli.Post(route.c_str(), headers, body_str, "application/json");
            if (res) break; // got an HTTP response (any status)
            lastErr = httplib::to_string(res.error());
            add("attempt " + std::to_string(attempt) + " transport error: " + lastErr + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
        }
        if (!res) return bail("transport failure: " + lastErr + " (after 3 attempts)");
        add("http: " + std::to_string(res->status) + "\nraw: " + res->body + "\n");
        if (res->status != 200) return bail("http status " + std::to_string(res->status));

        json reply = json::parse(res->body, nullptr, false);
        if (reply.is_discarded() || !reply.contains("choices") || !reply["choices"].is_array() ||
            reply["choices"].empty())
            return bail("unexpected response shape");
        const std::string content =
            reply["choices"][0].value("message", json::object()).value("content", std::string{});
        add("content: " + content + "\n");

        json parsed = json::parse(extractJsonObject(content), nullptr, false);
        if (parsed.is_discarded()) return bail("content was not JSON");

        ClassifyResult r;
        const std::string relStr = parsed.value("relation", std::string{"standalone"});
        r.relation = relationFromString(relStr);
        if (parsed.contains("targetId") && parsed["targetId"].is_number_unsigned())
            r.targetId = parsed["targetId"].get<TaskId>();
        r.confidence = parsed.value("confidence", 0.f);
        add("parsed: relation=" + relStr + " targetId=" + std::to_string(r.targetId) +
            " confidence=" + std::to_string(r.confidence) + "\n");

        if (r.relation == Relation::Standalone) return bail("model said standalone");
        if (r.targetId == 0) return bail("no targetId");
        if (r.confidence < confThreshold)
            return bail("confidence " + std::to_string(r.confidence) + " < threshold " +
                        std::to_string(confThreshold));
        if (L) {
            log += "RESULT: " + relStr + " targetId=" + std::to_string(r.targetId) + "\n";
            llmlog::write(log);
        }
        return r;
    } catch (const std::exception& e) {
        return bail(std::string("exception: ") + e.what());
    }
}

} // namespace

void OpenAiClassifier::classify(std::string newText, std::string existingTree,
                                ClassifyCallback done) {
    std::thread([=, endpoint = endpoint_, key = apiKey_, model = model_,
                 conf = confThreshold_, timeout = timeoutMs_]() {
        ClassifyResult r = run(endpoint, key, model, conf, timeout, newText, existingTree);
        if (done) done(r);
    }).detach();
}

} // namespace tt
