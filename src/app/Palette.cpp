#include "app/Palette.hpp"

#include <algorithm>
#include <cctype>

namespace tt::palette {
namespace {

std::string trim(const std::string& s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

// Digits -> id, with a cap so a pasted 30-digit number can't wrap around into a real id.
TaskId toId(const std::string& digits) {
    TaskId id = 0;
    for (char c : digits) {
        if (id > (1ull << 40)) return 0;   // absurd: no such node, treat as "none given"
        id = id * 10 + static_cast<TaskId>(c - '0');
    }
    return id;
}

} // namespace

const std::vector<ModeInfo>& modes() {
    static const std::vector<ModeInfo> kModes = {
        {Mode::Find, '?', "find", "highlight matching nodes and jump between them",
         "text to find…"},
        {Mode::Select, ':', "select", "select a node — its id, or text to match",
         "node id, or text to match…"},
        {Mode::Parent, '>', "parent", "make a node the parent of the selected one",
         "node id, or text to match…"},
    };
    return kModes;
}

const ModeInfo* infoFor(Mode m) {
    for (const ModeInfo& i : modes())
        if (i.mode == m) return &i;
    static const ModeInfo kMenu{Mode::Menu, '/', "mode", "pick what the bar does",
                                "pick a mode…"};
    return (m == Mode::Menu) ? &kMenu : nullptr;
}

Mode modeForPrefix(char c) {
    if (c == '/') return Mode::Menu;
    for (const ModeInfo& i : modes())
        if (i.prefix == c) return i.mode;
    return Mode::Add;
}

Command interpret(Mode mode, const std::string& arg) {
    Command c;
    switch (mode) {
        case Mode::Add:
            c.body = trim(arg);
            return c;
        case Mode::Menu:
            return c;                      // the menu itself does nothing until Enter
        case Mode::Find:
            c.kind = Kind::Find;
            c.query = trim(arg);
            return c;
        case Mode::Select:
        case Mode::Parent:
            break;
    }

    // Select / Parent: digits mean "by id", anything else is a text query. A leading '?'
    // forces text mode, so ':?12' looks for the text "12" instead of node 12.
    std::string tail = arg;
    const bool forcedText = !tail.empty() && tail[0] == '?';
    if (forcedText) tail.erase(0, 1);
    const std::string t = trim(tail);
    const bool byId = !forcedText && allDigits(t);

    if (mode == Mode::Select) c.kind = byId ? Kind::SelectId : Kind::SelectText;
    else                      c.kind = byId ? Kind::ParentId : Kind::ParentText;
    if (byId) c.id = toId(t);
    else      c.query = t;
    return c;
}

std::vector<TaskId> rankMatches(const Forest& f, const std::string& query, std::size_t limit) {
    std::vector<TaskId> out;
    const std::string q = lower(trim(query));
    if (q.empty()) return out;

    struct Hit {
        std::size_t pos;   // where the query matched (0 = the text starts with it)
        std::size_t len;   // node text length: a shorter text is the tighter match
        TaskId id;
    };
    std::vector<Hit> hits;
    for (const auto& [id, t] : f.nodes) {
        if (f.isInDoneSection(id)) continue;
        const std::string lt = lower(t.text);
        const std::size_t pos = lt.find(q);
        if (pos == std::string::npos) continue;
        hits.push_back({pos, lt.size(), id});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.len != b.len) return a.len < b.len;
        return a.id < b.id;
    });

    const std::size_t n = (limit == 0) ? hits.size() : std::min(limit, hits.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(hits[i].id);
    return out;
}

} // namespace tt::palette
