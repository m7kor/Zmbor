#include "runtime/pad_playback.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace
{
    bool number(std::string text, uint64_t &value)
    {
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            text.erase(0, 2);
            base = 16;
        }
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }
}

bool PadPlayback::load(std::istream &input, std::string &error)
{
    // Parse transactionally: a rejected file cannot leave a partial sequence.
    m_events = {};
    PadPlayback candidate;
    error.clear();
    std::string line;
    size_t lineNumber = 0, count = 0;
    uint64_t previousTick = 0;
    bool header = false;
    while (std::getline(input, line))
    {
        ++lineNumber;
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        std::istringstream row(line);
        std::string first;
        if (!(row >> first))
            continue;
        auto fail = [&](const char *reason)
        {
            error = "line " + std::to_string(lineNumber) + ": " + reason;
            return false;
        };
        if (!header)
        {
            std::string unit, extra;
            if (first != "timeline" || !(row >> unit) || (row >> extra) ||
                (unit != "frames" && unit != "milliseconds"))
                return fail("expected timeline frames|milliseconds");
            candidate.m_frames = unit == "frames";
            header = true;
            continue;
        }
        uint64_t values[8]{};
        if (!number(first, values[0]))
            return fail("invalid tick");
        for (size_t i = 1; i < 8; ++i)
        {
            std::string token;
            if (!(row >> token) || !number(token, values[i]))
                return fail("expected tick port slot pressedMask lx ly rx ry");
        }
        std::string extra;
        if ((row >> extra) || values[1] > 1 || values[2] != 0 || values[3] > 65535 ||
            values[4] > 255 || values[5] > 255 || values[6] > 255 || values[7] > 255)
            return fail("invalid port, slot, mask, axis or trailing data");
        if (count && values[0] < previousTick)
            return fail("ticks must be nondecreasing");
        if (++count > 100000)
            return fail("event limit exceeded");
        previousTick = values[0];
        Event event;
        event.tick = values[0];
        event.state.buttons = static_cast<uint16_t>(~values[3]);
        event.state.lx = static_cast<uint8_t>(values[4]);
        event.state.ly = static_cast<uint8_t>(values[5]);
        event.state.rx = static_cast<uint8_t>(values[6]);
        event.state.ry = static_cast<uint8_t>(values[7]);
        candidate.m_events[values[1]].push_back(event);
    }
    if (!header || input.bad())
    {
        error = !header ? "missing timeline header" : "script read failed";
        return false;
    }
    *this = std::move(candidate);
    return true;
}

PadPlayback::State PadPlayback::sample(int port, int slot, uint64_t frames, uint64_t milliseconds,
                                     const Event **selected) const
{
    if (selected)
        *selected = nullptr;
    if (port < 0 || port >= 2 || slot != 0)
        return {};
    const auto &events = m_events[port];
    const uint64_t tick = m_frames ? frames : milliseconds;
    auto it = std::upper_bound(events.begin(), events.end(), tick,
                               [](uint64_t tick, const Event &event) { return tick < event.tick; });
    if (it == events.begin())
        return {};
    --it;
    if (selected)
        *selected = &*it;
    return it->state;
}
