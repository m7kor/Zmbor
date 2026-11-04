#include "MiniTest.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "Stubs/Pad.h"
#include "runtime/pad_playback.h"

#include <vector>
#include <cstdint>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <chrono>


namespace
{
    constexpr uint32_t kPadDataAddr = 0x1000;

    constexpr uint16_t kPadBtnSelect = 1u << 0;
    constexpr uint16_t kPadBtnL3 = 1u << 1;
    constexpr uint16_t kPadBtnR3 = 1u << 2;
    constexpr uint16_t kPadBtnStart = 1u << 3;
    constexpr uint16_t kPadBtnUp = 1u << 4;
    constexpr uint16_t kPadBtnRight = 1u << 5;
    constexpr uint16_t kPadBtnDown = 1u << 6;
    constexpr uint16_t kPadBtnLeft = 1u << 7;
    constexpr uint16_t kPadBtnL2 = 1u << 8;
    constexpr uint16_t kPadBtnR2 = 1u << 9;
    constexpr uint16_t kPadBtnL1 = 1u << 10;
    constexpr uint16_t kPadBtnR1 = 1u << 11;
    constexpr uint16_t kPadBtnTriangle = 1u << 12;
    constexpr uint16_t kPadBtnCircle = 1u << 13;
    constexpr uint16_t kPadBtnCross = 1u << 14;
    constexpr uint16_t kPadBtnSquare = 1u << 15;

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void openPadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        setRegU32(ctx, 6, kPadDataAddr + 0x200u);
        ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
    }

    void closePadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        ps2_stubs::scePadPortClose(rdram.data(), &ctx, nullptr);
    }

    void runPadRead(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        setRegU32(ctx, 6, kPadDataAddr); // a2
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
    }

    uint16_t readButtons(const std::vector<uint8_t> &rdram)
    {
        const uint8_t *data = rdram.data() + kPadDataAddr;
        return static_cast<uint16_t>(data[2] | (data[3] << 8));
    }
}

void register_pad_input_tests()
{
    MiniTest::Case("PadInput", [](TestCase &tc)
                   {
        tc.Run("scripted frames preserve holds releases analog and independent ports", [](TestCase &t)
        {
            PadPlayback playback;
            std::istringstream script("# complete states\ntimeline frames\n"
                                      "10 0 0 0x4008 0 255 16 238\n"
                                      "10 1 0 0x80 128 128 128 128\n"
                                      "12 0 0 0 128 128 128 128\n");
            std::string error;
            t.IsTrue(playback.load(script, error), error);
            t.Equals(playback.sample(0, 0, 9, 90000).buttons, uint16_t{0xFFFF}, "before first frame is neutral regardless of wall time");
            const auto pressed = playback.sample(0, 0, 10, 0);
            t.Equals(pressed.buttons, uint16_t{0xBFF7}, "start and cross become active-low at boundary");
            t.Equals(pressed.lx, uint8_t{0}, "left stick low endpoint");
            t.Equals(pressed.ly, uint8_t{255}, "left stick high endpoint");
            t.Equals(pressed.rx, uint8_t{16}, "right stick X");
            t.Equals(pressed.ry, uint8_t{238}, "right stick Y");
            t.Equals(playback.sample(0, 0, 11, 0).buttons, pressed.buttons, "hold survives repeated and later polls");
            t.Equals(playback.sample(1, 0, 12, 0).buttons, uint16_t{0xFF7F}, "second port independent");
            t.Equals(playback.sample(0, 0, 12, 0).buttons, uint16_t{0xFFFF}, "release at exact frame");
            t.Equals(playback.sample(0, 0, 12, 0).lx, uint8_t{128}, "release state recenters stick");
            t.Equals(playback.sample(0, 1, 10, 0).buttons, uint16_t{0xFFFF}, "unsupported slot stays neutral");
            t.Equals(playback.sample(0, 0, 10, 0).buttons, pressed.buttons, "replaying earlier frame repeats identical state");
        });

        tc.Run("scripted milliseconds use time and last simultaneous event", [](TestCase &t)
        {
            PadPlayback playback;
            std::istringstream script("timeline milliseconds\n5 0 0 8 128 128 128 128\n5 0 0 1 128 128 128 128\n8 0 0 0 128 128 128 128\n");
            std::string error;
            t.IsTrue(playback.load(script, error), error);
            t.IsFalse(playback.usesFrames(), "milliseconds timeline selected");
            t.Equals(playback.sample(0, 0, 999, 4).buttons, uint16_t{0xFFFF}, "frames do not advance timed script");
            t.Equals(playback.sample(0, 0, 0, 5).buttons, uint16_t{0xFFFE}, "last same-tick event wins");
            t.Equals(playback.sample(0, 0, 0, 8).buttons, uint16_t{0xFFFF}, "timed release");
        });

        tc.Run("script parser rejects malformed input without partial playback", [](TestCase &t)
        {
            const char *invalid[] = {
                "", "timeline polls\n", "timeline frames extra\n",
                "timeline frames\n-1 0 0 8 128 128 128 128\n",
                "timeline frames\n0 2 0 8 128 128 128 128\n",
                "timeline frames\n0 0 1 8 128 128 128 128\n",
                "timeline frames\n0 0 0 65536 128 128 128 128\n",
                "timeline frames\n0 0 0 8 256 128 128 128\n",
                "timeline frames\n0 0 0 8 128 128 128\n",
                "timeline frames\n0 0 0 8 128 128 128 128 extra\n",
                "timeline frames\n9 0 0 8 128 128 128 128\n8 0 0 0 128 128 128 128\n"
            };
            for (const auto *text : invalid)
            {
                PadPlayback playback;
                std::istringstream valid("timeline frames\n0 0 0 8 128 128 128 128\n");
                std::string error;
                t.IsTrue(playback.load(valid, error), "seed sequence loads");
                std::istringstream script(text);
                t.IsFalse(playback.load(script, error), "malformed script rejected");
                t.IsFalse(error.empty(), "parse error explains failure");
                t.Equals(playback.sample(0, 0, 100, 100).buttons, uint16_t{0xFFFF}, "failed load clears previous/partial sequence");
            }
        });

        tc.Run("scePadRead consumes script and preserves explicit override precedence", [](TestCase &t)
        {
            struct ScriptEnvironment
            {
                std::string previous;
                bool hadPrevious = false;
                std::filesystem::path path = std::filesystem::temp_directory_path() /
                    ("zmbor-pad-playback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
                ScriptEnvironment()
                {
                    const char *old = std::getenv("ZMBOR_PAD_SCRIPT");
                    hadPrevious = old != nullptr;
                    if (old) previous = old;
                    std::ofstream(path) << "timeline frames\n0 0 0 0x4008 0 255 16 238\n";
#ifdef _WIN32
                    _putenv_s("ZMBOR_PAD_SCRIPT", path.string().c_str());
#else
                    setenv("ZMBOR_PAD_SCRIPT", path.string().c_str(), 1);
#endif
                }
                ~ScriptEnvironment()
                {
#ifdef _WIN32
                    _putenv_s("ZMBOR_PAD_SCRIPT", hadPrevious ? previous.c_str() : "");
#else
                    if (hadPrevious) setenv("ZMBOR_PAD_SCRIPT", previous.c_str(), 1);
                    else unsetenv("ZMBOR_PAD_SCRIPT");
#endif
                    std::error_code error;
                    std::filesystem::remove(path, error);
                }
            };
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            {
                ScriptEnvironment environment;
                ps2_stubs::clearPadOverrideState();
                ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
                openPadPort(ctx, rdram);
                runPadRead(ctx, rdram);
                t.Equals(readButtons(rdram), uint16_t{0xBFF7}, "script reaches guest pad bytes");
                t.Equals(rdram[kPadDataAddr + 4], uint8_t{16}, "script right X reaches guest");
                t.Equals(rdram[kPadDataAddr + 6], uint8_t{0}, "script left X reaches guest");
                ps2_stubs::setPadOverrideState(0xFFFE, 128, 128, 128, 128);
                runPadRead(ctx, rdram);
                t.Equals(readButtons(rdram), uint16_t{0xFFFE}, "explicit override retains precedence");
                ps2_stubs::clearPadOverrideState();
                runPadRead(ctx, rdram);
                t.Equals(readButtons(rdram), uint16_t{0xBFF7}, "clearing override resumes scripted timeline");
#ifdef _WIN32
                _putenv_s("ZMBOR_PAD_SCRIPT", "");
#else
                unsetenv("ZMBOR_PAD_SCRIPT");
#endif
                ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
                openPadPort(ctx, rdram);
                runPadRead(ctx, rdram);
                t.IsFalse(ps2_stubs::getPadDebugSnapshot().ports[0][0].lastUsedOverride,
                          "reinitializing without script restores normal input path");
            }
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
        });

        tc.Run("scePadRead uses override state", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross & ~kPadBtnStart);
            ps2_stubs::setPadOverrideState(buttons, 0x00, 0xFF, 0x10, 0xEE);

            runPadRead(ctx, rdram);

            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should return 1");
            t.Equals(readButtons(rdram), buttons, "button bitmask should match override state");
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[4], static_cast<uint8_t>(0x10), "rx should match override");
            t.Equals(data[5], static_cast<uint8_t>(0xEE), "ry should match override");
            t.Equals(data[6], static_cast<uint8_t>(0x00), "lx should match override");
            t.Equals(data[7], static_cast<uint8_t>(0xFF), "ly should match override");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead button bits are active-low", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            struct ButtonCase
            {
                uint16_t mask;
                const char *name;
            };

            const ButtonCase cases[] = {
                {kPadBtnSelect, "select"},
                {kPadBtnL3, "l3"},
                {kPadBtnR3, "r3"},
                {kPadBtnStart, "start"},
                {kPadBtnUp, "up"},
                {kPadBtnRight, "right"},
                {kPadBtnDown, "down"},
                {kPadBtnLeft, "left"},
                {kPadBtnL2, "l2"},
                {kPadBtnR2, "r2"},
                {kPadBtnL1, "l1"},
                {kPadBtnR1, "r1"},
                {kPadBtnTriangle, "triangle"},
                {kPadBtnCircle, "circle"},
                {kPadBtnCross, "cross"},
                {kPadBtnSquare, "square"}};

            for (const auto &entry : cases)
            {
                const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~entry.mask);
                ps2_stubs::setPadOverrideState(buttons, 0x80, 0x80, 0x80, 0x80);
                runPadRead(ctx, rdram);

                t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should succeed for opened ports");
                const uint16_t mask = readButtons(rdram);
                t.IsTrue((mask & entry.mask) == 0, std::string("button should be active-low: ").append(entry.name));
            }

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadGetButtonMask returns all buttons", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetButtonMask(nullptr, &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0xFFFF), "button mask should be 0xFFFF");
        });

        tc.Run("basic pad init/port/state functions return expected values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit should succeed");

            ps2_stubs::scePadInit2(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit2 should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should report DISCONNECTED");

            openPadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortOpen should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "scePadGetState should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadGetReqState should return completed");

            ps2_stubs::scePadGetPortMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(2), "scePadGetPortMax should be 2");

            setRegU32(ctx, 4, 0u);
            ps2_stubs::scePadGetSlotMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadGetSlotMax should be 1");

            ps2_stubs::scePadGetModVersion(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0x0200), "scePadGetModVersion should be 0x0200");

            closePadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortClose should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should return DISCONNECTED after close");
        });

        tc.Run("pad command state reports EXECCMD once before returning STABLE", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after mode command should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after press-mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after press-mode command should return STABLE");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad info and mode helpers return consistent values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 6, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoAct(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoAct should report at least one actuator descriptor");

            ps2_stubs::scePadInfoComb(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadInfoComb should return 0");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoMode table count should be non-zero");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadInfoPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInfoPressMode should report pressure support");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "CURID should switch to digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept analog mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "mode table entry should return DualShock in analog mode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pads open in digital mode and switch to analog on scePadSetMainMode", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "freshly opened port should report STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            runPadRead(ctx, rdram);
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[1], static_cast<uint8_t>(0x41), "mode byte should be 0x41 (digital) at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 3);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed switching to analog");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "scePadInfoMode CURID should return analog after SetMainMode");

            // scePadSetMainMode queues a one-shot EXECCMD transient state; pump scePadGetState
            // once so the port settles back to STABLE before reading, mirroring the existing
            // "pad command state reports EXECCMD once before returning STABLE" test.
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);

            runPadRead(ctx, rdram);
            t.Equals(data[1], static_cast<uint8_t>(0x73), "mode byte should be 0x73 (analog) after SetMainMode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad setters return success", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActAlign(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActAlign should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActDirect(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActDirect should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetReqState should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetVrefParam(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetVrefParam should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetWarningLevel(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadSetWarningLevel should return 0");

            ps2_stubs::scePadEnd(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnd should succeed");

            openPadPort(ctx, rdram);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadExitPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadExitPressMode should succeed");

            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead fills pressure bytes and honors button info mask", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should accept all buttons");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should enable pressure data");

            const uint16_t pressedButtons = static_cast<uint16_t>(0xFFFFu &
                                                                   ~kPadBtnLeft &
                                                                   ~kPadBtnUp &
                                                                   ~kPadBtnTriangle &
                                                                   ~kPadBtnCross &
                                                                   ~kPadBtnL1 &
                                                                   ~kPadBtnR2);
            ps2_stubs::setPadOverrideState(pressedButtons, 0x80, 0x80, 0x80, 0x80);
            runPadRead(ctx, rdram);

            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[8], static_cast<uint8_t>(0x00), "right pressure should be clear when not pressed");
            t.Equals(data[9], static_cast<uint8_t>(0xFF), "left pressure should be populated when pressed");
            t.Equals(data[10], static_cast<uint8_t>(0xFF), "up pressure should be populated when pressed");
            t.Equals(data[11], static_cast<uint8_t>(0x00), "down pressure should be clear when not pressed");
            t.Equals(data[12], static_cast<uint8_t>(0xFF), "triangle pressure should be populated when pressed");
            t.Equals(data[13], static_cast<uint8_t>(0x00), "circle pressure should be clear when not pressed");
            t.Equals(data[14], static_cast<uint8_t>(0xFF), "cross pressure should be populated when pressed");
            t.Equals(data[15], static_cast<uint8_t>(0x00), "square pressure should be clear when not pressed");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "L1 pressure should be populated when pressed");
            t.Equals(data[17], static_cast<uint8_t>(0x00), "L2 pressure should be clear when not pressed");
            t.Equals(data[18], static_cast<uint8_t>(0x00), "R1 pressure should be clear when not pressed");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "R2 pressure should be populated when pressed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, static_cast<uint32_t>(kPadBtnL1 | kPadBtnR2));
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should narrow the enabled pressure mask");

            runPadRead(ctx, rdram);

            t.Equals(data[9], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[10], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[12], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[14], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "enabled L1 pressure should remain populated");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "enabled R2 pressure should remain populated");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad string helpers map state codes", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr)).find("FINDPAD") != std::string::npos,
                     "state 1 should map to FINDPAD");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64)).find("DISCONNECTED") != std::string::npos,
                     "state 0 should map to DISCONNECTED");

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr + 128);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 128)).find("BUSY") != std::string::npos,
                     "req state 1 should map to BUSY");
        });
        tc.Run("scePadGetFrameCount increments", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t first = getRegU32(&ctx, 2);
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t second = getRegU32(&ctx, 2);
            t.Equals(second, first + 1, "frame count should increment");
        });

        tc.Run("scePadStateIntToStr and scePadReqIntToStr write strings", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 6);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            const char *stateStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr);
            t.IsTrue(std::string(stateStr).find("STABLE") != std::string::npos, "state string should include STABLE");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            const char *reqStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64);
            t.IsTrue(std::string(reqStr).find("COMPLETE") != std::string::npos, "req string should include COMPLETE");
        });
    });
}
