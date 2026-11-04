// TESTS ONLY. paraLLEl-GS adapter tests (GS Stage 2). These run only in
// builds with PS2X_ENABLE_PARALLEL_GS=ON; in default builds this file
// contributes no cases. They drive ParallelGSBackend directly (no frontend)
// on the lavapipe software Vulkan device, so they need no display.
//
// Packet encodings are copied from the Stage 1 gs_verify harness
// (register values, display setup, sprite geometry), not re-derived:
//   - display: pmode 0xFF25, smode2 0, dispfb1 = 10<<9 (FBW 10, CT32, FBP 0),
//     display1 = (3<<23) | (2559<<32) | (447<<44)  (640x448, MAGH 3)
//   - context: FRAME_1 = FBP 0 / FBW 10 / CT32, full scissor, XYOFFSET 0,
//     TEST_1 = ZTE + ZTST ALWAYS, ZBUF_1 = ZBP 0x100 / Z32 / ZMASK,
//     PRMODECONT = 1, PRIM = sprite.
//   - A+D packet: GIFtag(NLOOP=n, PACKED, NREG=1) + regs qword 0x0E +
//     (value, addr) qword pairs.
//   - PACKED vertex packet: GIFtag(NLOOP, PACKED, NREG) + regs qword
//     (4-bit register ids, low first) + NREG qwords per loop.

#include "MiniTest.h"

#if PS2X_HAS_PARALLEL_GS

#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

void put64(std::vector<uint8_t> &out, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint64_t gifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop)
{
    uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
    if (eop)
        tag |= (1ull << 15);
    tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
    tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
    return tag;
}

// PACKED A+D packet for a list of (addr, value) register writes.
std::vector<uint8_t> adPacket(const std::vector<std::pair<uint32_t, uint64_t>> &regs)
{
    std::vector<uint8_t> p;
    put64(p, gifTag(static_cast<uint16_t>(regs.size()), 0u, 1u, true));
    put64(p, 0x0Eull); // REGS[0] = A+D
    for (const auto &r : regs)
    {
        put64(p, r.second);
        put64(p, r.first);
    }
    return p;
}

uint64_t rgbaq(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 0x80)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// PACKED XYZ2 layout (parallel-gs PackedXYZBits, matching the PS2 PACKED
// format): X in bits 0-15, Y in bits 32-47 — each coordinate in its own
// 32-bit word of the 128-bit unit. This is NOT the A+D register layout
// (X 0-15, Y 16-31). Using the A+D layout here produced Y=0 vertices and
// a degenerate (zero-height) sprite that drew nothing.
uint64_t xyz2(uint32_t x, uint32_t y)
{
    return (static_cast<uint64_t>(x) << 4) | (static_cast<uint64_t>(y) << 36);
}

// Context setup + PRIM=sprite, as one A+D packet (path 3).
std::vector<uint8_t> setupPacket()
{
    return adPacket({
        {0x4C, 0u | (10ull << 16)},                       // FRAME_1
        {0x40, 0ull | (639ull << 16) | (447ull << 48)},   // SCISSOR_1
        {0x18, 0},                                        // XYOFFSET_1
        {0x47, (1ull << 16) | (1ull << 17)},              // TEST_1 ZTE+ALWAYS
        {0x4E, 0x100ull | (1ull << 32)},                 // ZBUF_1 ZBP 0x100, ZMASK
        {0x1A, 1},                                        // PRMODECONT
        {0x00, 6},                                        // PRIM = sprite
    });
}

// Flat red sprite, 100,100 -> 200,200, as one PACKED packet (path 3).
// PACKED mode consumes one 128-bit unit per register (see the frontend's
// processGIFPacket: offset += 16 per register), so each value is padded
// to a full unit: [tag, regs, RGBAQ, pad, XYZ2, pad, RGBAQ, pad, XYZ2, pad].
std::vector<uint8_t> spritePacket()
{
    std::vector<uint8_t> p;
    put64(p, gifTag(2u, 0u, 2u, true)); // PACKED, NLOOP 2, NREG 2
    put64(p, 0x01ull | (0x05ull << 4)); // REGS = RGBAQ, XYZ2
    put64(p, rgbaq(0xFF, 0x00, 0x00));
    put64(p, 0);
    put64(p, xyz2(100, 100));
    put64(p, 0);
    put64(p, rgbaq(0xFF, 0x00, 0x00));
    put64(p, 0);
    put64(p, xyz2(200, 200));
    put64(p, 0);
    return p;
}

GSPresentationRequest displayRequest(uint64_t tick)
{
    GSPresentationRequest rq{};
    rq.pmode = 0xFF25;
    rq.smode2 = 0;
    rq.dispfb1 = (10u << 9);
    rq.display1 = (3ull << 23) | (2559ull << 32) | (447ull << 44);
    rq.bgcolor = 0;
    rq.vsyncTick = tick;
    return rq;
}

bool pixelIs(const PresentationFrame &f, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= f.width || y >= f.height)
        return false;
    const size_t o = (static_cast<size_t>(y) * f.width + x) * 4;
    return f.pixels[o] == r && f.pixels[o + 1] == g && f.pixels[o + 2] == b;
}

struct BackendFixture
{
    std::vector<uint8_t> vram;
    ParallelGSBackend backend;

    BackendFixture() : vram(kVramSize, 0)
    {
        backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    }
};

} // namespace

#endif // PS2X_HAS_PARALLEL_GS

void register_ps2_parallel_gs_tests()
{
#if PS2X_HAS_PARALLEL_GS
    MiniTest::Case("PS2ParallelGS", [](TestCase &tc)
    {
    tc.Run("1 device initializes, reports raw-GIF mode, no error", [](TestCase &t)
    {
        BackendFixture fx;
        t.IsTrue(fx.backend.deviceReady(), "device should initialize on the available Vulkan device");
        t.IsTrue(fx.backend.WantsRawGif(), "adapter must report raw-GIF mode");
        t.IsTrue(fx.backend.LastError() == nullptr, "no error should be recorded after a clean init");
    });

    tc.Run("2 present without draws yields a 640x448 black frame", [](TestCase &t)
    {
        BackendFixture fx;
        if (!fx.backend.deviceReady())
        {
            t.Fail("device should initialize before the present test");
            return;
        }
        fx.backend.Flush();
        fx.backend.Sync(GSSyncReason::Presentation);
        PresentationFrame f = fx.backend.Present(displayRequest(0));
        t.IsTrue(static_cast<bool>(f), "present should return a frame");
        t.Equals(f.width, 640u, "frame width should be 640");
        t.Equals(f.height, 448u, "frame height should be 448");
        t.IsTrue(pixelIs(f, 0, 0, 0, 0, 0), "undrawn framebuffer should present black at (0,0)");
        t.IsTrue(pixelIs(f, 639, 447, 0, 0, 0), "undrawn framebuffer should present black at (639,447)");
    });

    tc.Run("3 flat sprite renders red at its interior, black outside", [](TestCase &t)
    {
        BackendFixture fx;
        if (!fx.backend.deviceReady())
        {
            t.Fail("device should initialize before the sprite test");
            return;
        }
        const std::vector<uint8_t> setup = setupPacket();
        const std::vector<uint8_t> sprite = spritePacket();
        fx.backend.RawGif(3, setup.data(), static_cast<uint32_t>(setup.size()));
        fx.backend.RawGif(3, sprite.data(), static_cast<uint32_t>(sprite.size()));
        fx.backend.Flush();
        fx.backend.Sync(GSSyncReason::Finish);
        PresentationFrame f = fx.backend.Present(displayRequest(0));
        t.IsTrue(static_cast<bool>(f), "present should return a frame after the sprite");
        t.Equals(f.width, 640u, "sprite frame width should be 640");
        t.Equals(f.height, 448u, "sprite frame height should be 448");
        t.IsTrue(pixelIs(f, 150, 150, 0xFF, 0x00, 0x00), "sprite interior (150,150) should be red");
        t.IsTrue(pixelIs(f, 0, 0, 0, 0, 0), "outside the sprite (0,0) should stay black");
        t.IsTrue(pixelIs(f, 250, 250, 0, 0, 0), "outside the sprite (250,250) should stay black");
    });

    tc.Run("4 split packet with interleaved path-1 packet renders identically", [](TestCase &t)
    {
        // Run A: uninterrupted sprite stream on path 3.
        BackendFixture fa;
        // Run B: same stream, but the vertex packet is split mid-loop
        // with a complete TEXFLUSH packet on path 1 interleaved. TEXFLUSH
        // touches no framebuffer state, so the frames must be identical
        // iff per-path GIF tag state stays separated.
        BackendFixture fb;
        if (!fa.backend.deviceReady() || !fb.backend.deviceReady())
        {
            t.Fail("devices should initialize before the interleave test");
            return;
        }
        const std::vector<uint8_t> setup = setupPacket();
        const std::vector<uint8_t> sprite = spritePacket();
        const std::vector<uint8_t> texflush = adPacket({{0x3F, 0}});

        fa.backend.RawGif(3, setup.data(), static_cast<uint32_t>(setup.size()));
        fa.backend.RawGif(3, sprite.data(), static_cast<uint32_t>(sprite.size()));
        fa.backend.Flush();
        fa.backend.Sync(GSSyncReason::Finish);
        PresentationFrame frameA = fa.backend.Present(displayRequest(0));

        fb.backend.RawGif(3, setup.data(), static_cast<uint32_t>(setup.size()));
        // Split after tag + regs + first RGBAQ unit (4 qwords = 64 bytes):
        // the split must land on a register boundary so each path's tag
        // state resumes cleanly.
        fb.backend.RawGif(3, sprite.data(), 64u);
        fb.backend.RawGif(1, texflush.data(), static_cast<uint32_t>(texflush.size()));
        fb.backend.RawGif(3, sprite.data() + 64, static_cast<uint32_t>(sprite.size()) - 64u);
        fb.backend.Flush();
        fb.backend.Sync(GSSyncReason::Finish);
        PresentationFrame frameB = fb.backend.Present(displayRequest(0));

        t.IsTrue(static_cast<bool>(frameA), "run A should present a frame");
        t.IsTrue(static_cast<bool>(frameB), "run B should present a frame");
        t.Equals(frameB.width, frameA.width, "interleaved run width should match");
        t.Equals(frameB.height, frameA.height, "interleaved run height should match");
        t.IsTrue(frameB.pixels == frameA.pixels,
                 "interleaved run pixels should be byte-identical to the uninterrupted run");
    });

    tc.Run("5 malformed RawGif input is rejected, never crashes, device survives", [](TestCase &t)
    {
        BackendFixture fx;
        if (!fx.backend.deviceReady())
        {
            t.Fail("device should initialize before the malformed-input test");
            return;
        }
        const std::vector<uint8_t> setup = setupPacket();
        fx.backend.RawGif(3, setup.data(), static_cast<uint32_t>(setup.size()));
        fx.backend.RawGif(0, setup.data(), static_cast<uint32_t>(setup.size()));
        t.IsTrue(fx.backend.LastError() != nullptr, "path 0 should be rejected with an error");
        fx.backend.RawGif(4, setup.data(), static_cast<uint32_t>(setup.size()));
        t.IsTrue(fx.backend.LastError() != nullptr, "path 4 should be rejected with an error");
        const uint8_t odd[8] = {};
        fx.backend.RawGif(3, odd, 8u);
        t.IsTrue(fx.backend.LastError() != nullptr, "non-16-multiple size should be rejected with an error");
        fx.backend.RawGif(3, nullptr, 0u);
        // Null/empty input is ignored, not an error: it carries no packet.
        fx.backend.Flush();
        fx.backend.Sync(GSSyncReason::Finish);
        PresentationFrame f = fx.backend.Present(displayRequest(0));
        t.IsTrue(static_cast<bool>(f), "device should still present after malformed input");
    });

    tc.Run("6 VRAM mirror round-trips through Sync and SnapshotVram", [](TestCase &t)
    {
        BackendFixture fx;
        if (!fx.backend.deviceReady())
        {
            t.Fail("device should initialize before the mirror test");
            return;
        }
        const std::vector<uint8_t> setup = setupPacket();
        const std::vector<uint8_t> sprite = spritePacket();
        fx.backend.RawGif(3, setup.data(), static_cast<uint32_t>(setup.size()));
        fx.backend.RawGif(3, sprite.data(), static_cast<uint32_t>(sprite.size()));
        fx.backend.Flush();
        fx.backend.Sync(GSSyncReason::Finish);
        std::vector<uint8_t> snap;
        fx.backend.SnapshotVram(snap);
        t.Equals(snap.size(), static_cast<size_t>(kVramSize), "snapshot should cover the full 4 MiB VRAM");
        // The sprite's pixels must be visible in the synced mirror: read
        // the framebuffer pixel at (150,150) in CT32 (FBP 0, FBW 10).
        // CT32 stores R in bits 0-7, G in 8-15, B in 16-23 (alpha in
        // bits 24-31, per the CPU backend's destination-alpha test), so a
        // pure red pixel reads 0x800000FF; assert the RGB bytes only,
        // since the adapter passes alpha through from the scanout path.
        const uint32_t px = fx.backend.ReadVram(0u, 0u, 10u, 150u, 150u);
        t.Equals(px & 0xFFu, 0xFFu, "synced mirror pixel (150,150) R byte should be 0xFF");
        t.Equals((px >> 8) & 0xFFu, 0u, "synced mirror pixel (150,150) G byte should be 0");
        t.Equals((px >> 16) & 0xFFu, 0u, "synced mirror pixel (150,150) B byte should be 0");
        t.IsTrue(snap == fx.vram, "borrowed VRAM buffer should match the snapshot after Sync");
    });
    });
#endif // PS2X_HAS_PARALLEL_GS
}
