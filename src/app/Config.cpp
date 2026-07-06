#include "app/Config.hpp"

#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/Paths.hpp"

namespace tt {

Config loadConfig(const std::string& path) {
    Config c;
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const std::exception&) {
        return c; // missing or invalid -> defaults
    }

    auto str = [&](std::string p, const std::string& def) {
        return tbl.at_path(p).value<std::string>().value_or(def);
    };
    auto flt = [&](std::string p, float def) {
        return static_cast<float>(tbl.at_path(p).value<double>().value_or(def));
    };
    auto integer = [&](std::string p, int def) {
        return static_cast<int>(tbl.at_path(p).value<int64_t>().value_or(def));
    };
    auto boolean = [&](std::string p, bool def) {
        return tbl.at_path(p).value<bool>().value_or(def);
    };
    auto col = [&](std::string p, Color def) {
        auto s = tbl.at_path(p).value<std::string>();
        return s ? colorFromHex(*s, def) : def;
    };

    c.toggleHotkey   = str("hotkeys.toggle", c.toggleHotkey);
    c.quickAddHotkey = str("hotkeys.quick_add", c.quickAddHotkey);

    c.maxNodeWidth = flt("layout.max_node_width", c.maxNodeWidth);
    c.cornerRadius = flt("layout.corner_radius", c.cornerRadius);
    c.borderWidth  = flt("layout.border_width", c.borderWidth);

    c.overlayOpacity = flt("theme.overlay_opacity", c.overlayOpacity);
    c.scrim        = col("theme.scrim", c.scrim);
    c.nodeFill     = col("theme.node_fill", c.nodeFill);
    c.nodeBorder   = col("theme.node_border", c.nodeBorder);
    c.nodeText     = col("theme.node_text", c.nodeText);
    c.edgeColor    = col("theme.edge", c.edgeColor);
    c.dropHint     = col("theme.drop_hint", c.dropHint);
    c.quickAddFill = col("theme.quick_add_fill", c.quickAddFill);

    c.llmEnabled  = boolean("llm.enabled", c.llmEnabled);
    c.llmEndpoint = str("llm.endpoint", c.llmEndpoint);
    c.llmModel    = str("llm.model", c.llmModel);
    c.llmConfidenceThreshold = flt("llm.confidence_threshold", c.llmConfidenceThreshold);
    c.llmTimeoutMs = integer("llm.timeout_ms", c.llmTimeoutMs);
    return c;
}

bool saveConfig(const Config& c, const std::string& path) {
    std::filesystem::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);

    std::ostringstream o;
    o << "# TaskTree configuration. Edit and restart to apply.\n\n"
      << "[hotkeys]\n"
      << "# Global hotkeys. Modifiers: Ctrl, Alt, Shift, Super. Avoid Super combos\n"
      << "# (the GNOME/mutter compositor grabs most of them).\n"
      << "toggle    = \"" << c.toggleHotkey << "\"   # show/hide the full overlay\n"
      << "quick_add = \"" << c.quickAddHotkey << "\" # show just the quick-add box\n\n"
      << "[layout]\n"
      << "max_node_width = " << c.maxNodeWidth << "  # px; text wraps, height grows to fit\n"
      << "corner_radius  = " << c.cornerRadius << "\n"
      << "border_width   = " << c.borderWidth << "\n\n"
      << "[theme]\n"
      << "overlay_opacity = " << c.overlayOpacity << "  # scrim darkness (0..1)\n"
      << "scrim           = \"" << colorToHex(c.scrim) << "\"\n"
      << "node_fill       = \"" << colorToHex(c.nodeFill) << "\"\n"
      << "node_border     = \"" << colorToHex(c.nodeBorder) << "\"\n"
      << "node_text       = \"" << colorToHex(c.nodeText) << "\"\n"
      << "edge            = \"" << colorToHex(c.edgeColor) << "\"\n"
      << "drop_hint       = \"" << colorToHex(c.dropHint) << "\"\n"
      << "quick_add_fill  = \"" << colorToHex(c.quickAddFill) << "\"\n\n"
      << "[llm]\n"
      << "# Optional local classifier. When disabled, every new task is standalone.\n"
      << "# To enable: install ollama, `ollama pull " << c.llmModel << "`, set enabled = true.\n"
      << "enabled              = " << (c.llmEnabled ? "true" : "false") << "\n"
      << "endpoint             = \"" << c.llmEndpoint << "\"\n"
      << "model                = \"" << c.llmModel << "\"\n"
      << "confidence_threshold = " << c.llmConfidenceThreshold << "\n"
      << "timeout_ms           = " << c.llmTimeoutMs << "\n";

    const std::filesystem::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << o.str();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

Config loadOrCreateDefaultConfig() {
    const std::string path = paths::configFile().string();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        Config def;
        saveConfig(def, path);
        return def;
    }
    return loadConfig(path);
}

} // namespace tt
