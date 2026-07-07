#include "llm/OpenAiClassifier.hpp"

#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

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
                   const std::string& newText,
                   const std::vector<std::pair<TaskId, std::string>>& existing) {
    ClassifyResult fallback; // Standalone
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

        std::string listing;
        for (const auto& [id, text] : existing) {
            listing += std::to_string(id);
            listing += ": ";
            listing += text;
            listing += '\n';
        }

        const std::string sys =
            "You organise tasks into a tree. Given a NEW task and a list of EXISTING "
            "tasks (id: text), decide the relationship. Reply with ONLY JSON: "
            "{\"relation\":\"standalone|child_of|parent_of\",\"targetId\":<id or null>,"
            "\"confidence\":<0..1>}. Use child_of when the new task belongs under an "
            "existing task, parent_of when existing tasks belong under the new one, "
            "otherwise standalone.";
        const std::string user =
            "NEW task:\n" + newText + "\n\nEXISTING tasks:\n" + (listing.empty() ? "(none)" : listing);

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
        auto res = cli.Post((prefix + "/chat/completions").c_str(), headers,
                            body.dump(), "application/json");
        if (!res || res->status != 200) return fallback;

        json reply = json::parse(res->body, nullptr, false);
        if (reply.is_discarded() || !reply.contains("choices") || !reply["choices"].is_array() ||
            reply["choices"].empty())
            return fallback;
        const std::string content =
            reply["choices"][0].value("message", json::object()).value("content", std::string{});

        json parsed = json::parse(extractJsonObject(content), nullptr, false);
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

void OpenAiClassifier::classify(std::string newText,
                                std::vector<std::pair<TaskId, std::string>> existing,
                                ClassifyCallback done) {
    std::thread([=, endpoint = endpoint_, key = apiKey_, model = model_,
                 conf = confThreshold_, timeout = timeoutMs_]() {
        ClassifyResult r = run(endpoint, key, model, conf, timeout, newText, existing);
        if (done) done(r);
    }).detach();
}

} // namespace tt
