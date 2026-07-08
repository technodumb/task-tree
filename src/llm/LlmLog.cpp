#include "llm/LlmLog.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>

namespace tt::llmlog {
namespace {

std::mutex g_mutex;
std::string g_path;
bool g_enabled = false;

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

void configure(bool enabled, std::string path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = enabled;
    g_path = std::move(path);
}

bool enabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled;
}

const std::string& path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void write(const std::string& body) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled || g_path.empty()) return;
    std::ofstream f(g_path, std::ios::app);
    if (!f) return;
    f << "==== " << timestamp() << " ====\n" << body << "\n";
}

} // namespace tt::llmlog
