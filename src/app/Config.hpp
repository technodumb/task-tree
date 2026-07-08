#pragma once
// User configuration (hotkeys, opacity, colours, LLM endpoint). Hand-editable TOML
// at $XDG_CONFIG_HOME/tasktree/config.toml. Missing keys fall back to defaults; a
// default file is written on first run.

#include <string>

#include "platform/Hotkey.hpp"
#include "render/Color.hpp"

namespace tt {

struct Config {
    // hotkeys
    std::string toggleHotkey   = "Ctrl+Alt+Space";
    std::string quickAddHotkey = "Ctrl+Alt+Return";

    // layout / node appearance
    float maxNodeWidth = 280.f;
    float cornerRadius = 14.f;
    float borderWidth  = 1.5f;

    // input: what the mouse wheel does over the canvas.
    //   "zoom" = wheel zooms about the cursor (no modifier needed)
    //   "pan"  = wheel pans vertically, Shift+wheel horizontally, Ctrl+wheel zooms
    //   "off"  = wheel does nothing on the canvas (drag to pan still works)
    std::string scrollMode = "zoom";

    // theme (RGBA 0..1)
    float overlayOpacity = 0.35f;         // scrim alpha in full-overlay mode
    Color scrim       {0.04f, 0.05f, 0.07f, 1.f}; // multiplied by overlayOpacity
    Color nodeFill    {0.13f, 0.15f, 0.19f, 0.96f};
    Color nodeBorder  {0.45f, 0.55f, 0.72f, 1.f};
    Color nodeText    {0.92f, 0.94f, 0.98f, 1.f};
    Color nodeTextDark{0.14f, 0.11f, 0.04f, 1.f};   // text on the bright status fills
    Color nodeFillInProgress{0.85f, 0.72f, 0.20f, 0.97f}; // right-click 1: yellow
    Color nodeFillPriority  {0.90f, 0.52f, 0.16f, 0.97f}; // right-click 2: orange
    Color idBadgeBg   {0.24f, 0.30f, 0.46f, 1.f};   // node id label background
    Color idBadgeText {0.86f, 0.90f, 1.00f, 1.f};
    Color edgeColor   {0.50f, 0.58f, 0.70f, 0.85f};
    Color dropHint    {0.40f, 0.80f, 0.55f, 0.95f}; // preview edge / gap cue
    Color quickAddFill{0.14f, 0.16f, 0.20f, 0.98f};

    // DONE section (greenish, translucent tint — no card borders)
    Color donePanelBg  {0.05f, 0.15f, 0.09f, 0.55f};
    Color doneCardFill {0.15f, 0.32f, 0.20f, 0.90f}; // used only for the PIN button
    Color doneCardBorder{0.28f, 0.68f, 0.44f, 1.f};  // used only for the PIN button
    Color doneText     {0.86f, 0.97f, 0.89f, 1.f};
    Color doneTitle    {0.55f, 0.94f, 0.68f, 1.f};

    // local LLM classifier (disabled by default)
    bool        llmEnabled  = false;
    std::string llmEndpoint = "http://localhost:11434";
    std::string llmModel    = "llama3.2";
    float       llmConfidenceThreshold = 0.55f;
    int         llmTimeoutMs = 4000;
    bool        llmLogRequests = true;   // log requests/responses to <data>/llm.log

    HotkeySpec toggleSpec()   const { return parseHotkey(toggleHotkey); }
    HotkeySpec quickAddSpec() const { return parseHotkey(quickAddHotkey); }
};

// Load config from `path`; returns defaults (unchanged) if the file is absent.
Config loadConfig(const std::string& path);

// Write `cfg` to `path` (with explanatory comments), creating parent dirs.
bool saveConfig(const Config& cfg, const std::string& path);

// Load from the XDG config path, writing a commented default file if none exists.
Config loadOrCreateDefaultConfig();

} // namespace tt
