#pragma once
// Compact, human date for tiny UI chips (the node band, ttd 143/145): "jul 27" when the
// year is the current one, "jul 27 '25" otherwise, and "" for 0/negative — createdAt 0
// means unknown (legacy rows) and done_at -1 means done-date-unknown, and a chip that
// cannot be truthful should be absent, not guessed.
//
// Pure text: tt_core owns no clock, so the caller passes `nowMs` (the same convention as
// Forest::addTask / markDone). Local time, English month abbreviations.

#include <cstdint>
#include <ctime>
#include <string>

namespace tt {

inline std::string shortDate(std::int64_t ms, std::int64_t nowMs) {
    if (ms <= 0) return {};
    static const char* const kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::time_t when = static_cast<std::time_t>(ms / 1000);
    const std::time_t now = static_cast<std::time_t>(nowMs / 1000);
    std::tm w{}, n{};
    localtime_r(&when, &w);
    localtime_r(&now, &n);
    std::string out = std::string(kMon[w.tm_mon]) + ' ' + std::to_string(w.tm_mday);
    if (w.tm_year != n.tm_year) {
        const int yy = ((w.tm_year % 100) + 100) % 100;   // works before 2000 too
        out += " '";
        if (yy < 10) out += '0';
        out += std::to_string(yy);
    }
    return out;
}

} // namespace tt
