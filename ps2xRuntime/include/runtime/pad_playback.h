#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

// A complete pad state persists until the next event for that port. Button bits
// in script files are active-high; the returned PS2 state is active-low.
class PadPlayback
{
public:
    struct State
    {
        uint16_t buttons = 0xFFFF;
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    };
    struct Event
    {
        uint64_t tick = 0;
        State state;
    };

    bool load(std::istream &input, std::string &error);
    State sample(int port, int slot, uint64_t frames, uint64_t milliseconds,
                 const Event **selected = nullptr) const;
    bool usesFrames() const noexcept { return m_frames; }

private:
    bool m_frames = true;
    std::array<std::vector<Event>, 2> m_events;
};
