#pragma once
// XDG base-directory helpers. Header-only; used by Store (data) and Config (config).

#include <cstdlib>
#include <filesystem>
#include <string>

namespace tt::paths {

inline std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

inline std::string home() { return envOr("HOME", "."); }

// $XDG_DATA_HOME/tasktree  (default ~/.local/share/tasktree)
inline std::filesystem::path dataDir() {
    std::string base = envOr("XDG_DATA_HOME", home() + "/.local/share");
    return std::filesystem::path(base) / "tasktree";
}

// $XDG_CONFIG_HOME/tasktree  (default ~/.config/tasktree)
inline std::filesystem::path configDir() {
    std::string base = envOr("XDG_CONFIG_HOME", home() + "/.config");
    return std::filesystem::path(base) / "tasktree";
}

inline std::filesystem::path tasksFile()  { return dataDir()   / "tasks.json"; }
inline std::filesystem::path configFile() { return configDir() / "config.toml"; }

// Create a directory (and parents) if missing. Best-effort; returns success.
inline bool ensureDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) return true;
    return std::filesystem::create_directories(dir, ec);
}

} // namespace tt::paths
