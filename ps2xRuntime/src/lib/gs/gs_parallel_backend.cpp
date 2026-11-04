// paraLLEl-GS Vulkan GS backend adapter (GS Stage 2). See
// gs_parallel_backend.h for the design contract.
//
// Key facts reused from parallel-gs itself (patterns, not copied code):
// - Standalone device setup mirrors sandbox/gs_test.cpp (Context loader,
//   instance+device, 4 frame contexts) EXCEPT the creation flags: the
//   sandbox passes push-descriptor + descriptor-heap + descriptor-buffer,
//   but on this VM's lavapipe (Mesa 24.0.5) that combination segfaults
//   inside libvulkan_lvp.so as soon as a real draw is submitted (upstream's
//   own parallel-gs-sandbox binary crashes the same way; the no-draw
//   smoke test passes because no draw work executes). Push-descriptor
//   alone renders correctly on lavapipe, so v1 requests only that flag.
//   Descriptor buffers are a performance feature, not a correctness
//   requirement, and device creation with them succeeds on lavapipe — the
//   crash is asynchronous during rendering, so there is no clean
//   try-and-fall-back signal; requesting them unconditionally would be
//   wrong here. Revisit when the driver is known-good.
// - Scanout readback mirrors tools/gs_repro_replayer.cpp: vsync with
//   dst_layout READ_ONLY_OPTIMAL, barrier to TRANSFER_SRC_OPTIMAL,
//   copy into a CachedHost buffer, COPY->HOST barrier, submit, wait_idle,
//   map_host_buffer(READ).
// - gif_transfer path_index is per-path GIF tag state only (no semantic
//   difference between slots), so the Zmbor path numbers 1/2/3 map
//   identically onto parallel-gs slots 1/2/3 (slot 0 unused).
//
// Presentation (v1) is a 1:1 VRAM-derived frame decoded from the synced
// mirror (see Present()): the parallel-gs scanout image applies CRTC
// magnification (MAGH/DW sampling), which the CPU backend's present does
// not, so scanout pixels cannot be compared against CPU goldens. Measured
// on lavapipe with a 100x100 sprite at (100,100): MAGH=3 scanout gives a
// solid 198x100 rect at (42,100)-(239,199); MAGH=0 gives exactly 10000 red
// pixels spread over a 370x132 bbox. The render itself is exact in both
// cases (mirror: 10000 nonzero words). Scanout geometry is a separate
// display-model question for Stage 3, reported via the sidecar, not gated
// in E2; E2 frames compare the 1:1 present both backends produce.

#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"

#include "gs_interface.hpp"
#include "buffer.hpp"
#include "command_buffer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "memory_allocator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{

// PSM dispatch for ReadVram/WriteVram over the synced mirror, using the
// same shared GSMem helpers as the CPU backend's handler table.
uint32_t readPixel(uint32_t psm, uint8_t *data, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm & 0x3Fu)
    {
    case GS_PSM_CT32: return GSMem::ReadCT32(data, base, bw, x, y);
    case GS_PSM_CT24: return GSMem::ReadCT24(data, base, bw, x, y);
    case GS_PSM_CT16: return GSMem::ReadCT16(data, base, bw, x, y);
    case GS_PSM_CT16S: return GSMem::ReadCT16S(data, base, bw, x, y);
    case GS_PSM_T8: return GSMem::ReadP8(data, base, bw, x, y);
    case GS_PSM_T8H: return GSMem::ReadP8H(data, base, bw, x, y);
    case GS_PSM_T4: return GSMem::ReadP4(data, base, bw, x, y);
    case GS_PSM_T4HL: return GSMem::ReadP4HL(data, base, bw, x, y);
    case GS_PSM_T4HH: return GSMem::ReadP4HH(data, base, bw, x, y);
    case GS_PSM_Z32: return GSMem::ReadZ32(data, base, bw, x, y);
    case GS_PSM_Z24: return GSMem::ReadZ24(data, base, bw, x, y);
    case GS_PSM_Z16: return GSMem::ReadZ16(data, base, bw, x, y);
    case GS_PSM_Z16S: return GSMem::ReadZ16S(data, base, bw, x, y);
    default: return GSMem::ReadNull(data, base, bw, x, y);
    }
}

void writePixel(uint32_t psm, uint8_t *data, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    switch (psm & 0x3Fu)
    {
    case GS_PSM_CT32: GSMem::WriteCT32(data, base, bw, x, y, value); break;
    case GS_PSM_CT24: GSMem::WriteCT24(data, base, bw, x, y, value); break;
    case GS_PSM_CT16: GSMem::WriteCT16(data, base, bw, x, y, value); break;
    case GS_PSM_CT16S: GSMem::WriteCT16S(data, base, bw, x, y, value); break;
    case GS_PSM_T8: GSMem::WriteP8(data, base, bw, x, y, value); break;
    case GS_PSM_T8H: GSMem::WriteP8H(data, base, bw, x, y, value); break;
    case GS_PSM_T4: GSMem::WriteP4(data, base, bw, x, y, value); break;
    case GS_PSM_T4HL: GSMem::WriteP4HL(data, base, bw, x, y, value); break;
    case GS_PSM_T4HH: GSMem::WriteP4HH(data, base, bw, x, y, value); break;
    case GS_PSM_Z32: GSMem::WriteZ32(data, base, bw, x, y, value); break;
    case GS_PSM_Z24: GSMem::WriteZ24(data, base, bw, x, y, value); break;
    case GS_PSM_Z16: GSMem::WriteZ16(data, base, bw, x, y, value); break;
    case GS_PSM_Z16S: GSMem::WriteZ16S(data, base, bw, x, y, value); break;
    default: GSMem::WriteNull(data, base, bw, x, y, value); break;
    }
}

// --- 1:1 presentation decode (port of the CPU backend's present logic) --
// Present() decodes the synced mirror exactly the way the CPU backend
// decodes its VRAM: same display-size, DISPFB, PMODE/SMODE2, preferred-
// source, black-frame fallback, field, and dual-CRT blend rules. The only
// difference is the pixel source (mirror instead of CPU VRAM). Host frame
// geometry matches the CPU backend: 640x512 RGBA buffer, rows strided by
// 640 pixels; the frame's width/height are the DISPLAY size.
constexpr uint32_t kHostFrameWidth = 640u;
constexpr uint32_t kHostFrameHeight = 512u;
constexpr uint32_t kDefaultDisplayWidth = 640u;
constexpr uint32_t kDefaultDisplayHeight = 448u;

struct PresentFrameReg
{
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint8_t psm = 0;
};

struct PresentReadOrigin
{
    uint32_t x = 0;
    uint32_t y = 0;
};

void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
{
    const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
    const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
    const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
    outWidth = (dw + 1u) / (magh + 1u);
    outHeight = dh + 1u;
    if (outWidth < 64u || outHeight < 64u)
    {
        outWidth = kDefaultDisplayWidth;
        outHeight = kDefaultDisplayHeight;
    }
    outWidth = outWidth < kHostFrameWidth ? outWidth : kHostFrameWidth;
    outHeight = outHeight < kHostFrameHeight ? outHeight : kHostFrameHeight;
}

PresentFrameReg decodeDisplayFrame(uint64_t dispfb64)
{
    PresentFrameReg frame{};
    frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
    frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
    frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
    return frame;
}

PresentReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
{
    return { static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
             static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu) };
}

bool hasDisplaySetup(uint64_t display64, const PresentFrameReg &frame)
{
    const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
    const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
    const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
    return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
}

// Decodes one framebuffer region from the mirror into a full host-sized
// RGBA buffer (zero-filled outside width x height). Returns false (and
// clears the output) for unsupported display PSMs, mirroring the CPU
// backend's CopyFrameToHostRgba local-memory path.
bool copyFrameToHostRgba(const uint8_t *mirror, const PresentFrameReg &frame,
                         uint32_t width, uint32_t height, std::vector<uint8_t> &outPixels,
                         bool preserveAlpha, uint32_t sourceOriginX, uint32_t sourceOriginY)
{
    if (!mirror)
        return false;
    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t basePtr = GSInternal::framePageBaseToBlock(frame.fbp);
    const uint32_t fbw = frame.fbw != 0u ? frame.fbw : kHostFrameWidth / 64u;
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                const uint32_t color = readPixel(frame.psm, const_cast<uint8_t *>(mirror), basePtr, fbw, sx, sy);
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = (preserveAlpha && frame.psm != GS_PSM_CT24)
                                       ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                const uint32_t color = readPixel(frame.psm, const_cast<uint8_t *>(mirror), basePtr, fbw, sx, sy);
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
{
    uint32_t count = 0u;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                ++count;
        }
    }
    return count;
}

void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
{
    if (pixels.empty() || width == 0u || height < 2u)
        return;
    const std::vector<uint8_t> source = pixels;
    for (uint32_t y = 0; y < height; ++y)
    {
        uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
        if (sourceY >= height)
            sourceY = height - 1u;
        std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                    source.data() + sourceY * kHostFrameWidth * 4u,
                    width * 4u);
    }
}

void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
            row[x * 4u + 3u] = 255u;
    }
}

uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
{
    const int delta = static_cast<int>(src) - static_cast<int>(dst);
    return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
}

// Video-mode fallback (see applyPrivRegs): parallel-gs's scanout only
// recognizes a fixed (CMOD, LC) table (NTSC/PAL/HDTV/VGA/UNKNOWN_29). A
// guest that never writes SMODE1 — including Zmbor's own e2e gs_sprite
// program, which sets only PMODE/DISPFB/DISPLAY — leaves SMODE1 = 0, which
// parallel-gs rejects with "Unknown video format" and no scanout image.
// The minimal fallback is INT-aware, because the table treats progressive
// and interlaced differently:
// - Progressive (INT=0): SMODE1 = 0 already carries CMOD = 0
//   (progressive), and the unrecognized part is LC = 0, which is not a
//   defined line-clock value. Substituting LC_ANALOG (32) while preserving
//   CMOD keeps the guest's mode (progressive NTSC for the 640x448
//   fixtures: mode 224 doubled to 448) instead of inventing a different
//   one — substituting CMOD = NTSC was tried first and is WRONG for
//   progressive content: it makes parallel-gs treat the signal as 240p
//   double-strike (224 lines, double_strike flag) and squash the 448-line
//   framebuffer.
// - Interlaced (INT=1): CMOD = 0 (progressive) + INT is NOT a recognized
//   pair at all (NTSC interlaced requires CMOD = NTSC (2)), so LC-only
//   substitution still yields "Unknown video format" and no scanout.
//   Substituting CMOD = NTSC (2) + LC_ANALOG gives standard NTSC
//   interlaced, the only analog interlaced mode in the table.
// The fallback applies to UNRECOGNIZED pairs only; recognized pairs pass
// through untouched. It is logged (once per distinct unrecognized
// (CMOD, LC, INT) triple) so E2 runs disclose it, and it is an adapter
// model choice, not a hardware claim.
bool videoModeRecognized(uint64_t smode1, uint64_t smode2)
{
    const uint64_t lc = (smode1 >> 3) & 0x7Full;
    const uint64_t cmod = (smode1 >> 13) & 0x3ull;
    const bool interlaced = (smode2 & 1u) != 0u;
    if (lc == 29u)
        return true; // LC_UNKNOWN_29: recognized for any CMOD.
    if (lc != 32u && lc != 22u && lc != 15u)
        return false;
    if (lc == 32u)
    {
        if (cmod == 2u)
            return true; // NTSC.
        if (cmod == 0u && !interlaced)
            return true; // NTSC progressive.
        if (cmod == 3u)
            return true; // PAL.
        return false;
    }
    // LC_HDTV (22) and LC_VGA (15) require CMOD_PROGRESSIVE.
    return cmod == 0u;
}

uint64_t fallbackSmode1(uint64_t smode1, uint64_t smode2)
{
    // LC (bits 3-9) -> LC_ANALOG; for interlaced content CMOD (bits 13-14)
    // -> NTSC as well (see the fallback comment above). All other bits
    // preserved.
    uint64_t out = (smode1 & ~(0x7Full << 3)) | (32ull << 3);
    if ((smode2 & 1u) != 0u)
        out = (out & ~(0x3ull << 13)) | (2ull << 13);
    return out;
}
// The priv structs use uint32_t bitfields and are 8 or 12 bytes (FIELD32
// padding can spill a struct past 8 bytes, e.g. SMODE2Bits is 12), but an
// audit of every priv struct in gs_registers.hpp shows all NAMED fields
// live in the first 8 bytes (the tail is unnamed padding, never read).
// Zero-filling first keeps that padding deterministic instead of stale.
template <typename T>
void assignRegBits(T &field, uint64_t value)
{
    std::memset(&field, 0, sizeof(field));
    std::memcpy(&field, &value, sizeof(uint64_t));
}

} // namespace

ParallelGSBackend::ParallelGSBackend() = default;

ParallelGSBackend::~ParallelGSBackend()
{
    // Tear down in reverse creation order. The Granite device must be
    // destroyed before its context, and the GSInterface before the device.
    m_iface.reset();
    m_device.reset();
    m_context.reset();
}

void ParallelGSBackend::setError(const std::string &message)
{
    m_lastError = message;
}

const char *ParallelGSBackend::LastError() const
{
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

bool ParallelGSBackend::deviceReady() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_deviceReady;
}

std::vector<ParallelGSBackend::ScanoutObservation> ParallelGSBackend::scanoutObservations() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_scanouts;
}

void ParallelGSBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Re-initialization tears the old device down first.
    m_iface.reset();
    m_device.reset();
    m_context.reset();
    m_deviceReady = false;
    m_lastError.clear();

    m_vramBorrowed = vram;
    m_vramBorrowedSize = vramSize;
    m_vramMirror.assign(vramSize, 0);
    // The shared GSMem pixel helpers read through lookup tables that the
    // CPU backend normally initializes in its own constructor. The adapter
    // must not depend on the CPU backend ever being constructed, and the
    // init is idempotent (pure table fills), so initialize them here.
    GSMem::InitLookupTables();
    m_scanouts.clear();

    if (const char *logPath = std::getenv("PS2X_PGS_SCANOUT_LOG"))
        m_scanoutLogPath = logPath;
    else
        m_scanoutLogPath.clear();

    if (!vram || vramSize == 0u)
    {
        setError("ParallelGSBackend::Initialize: null/empty borrowed VRAM");
        return;
    }

    try
    {
        if (!Vulkan::Context::init_loader(nullptr))
        {
            setError("ParallelGSBackend: Vulkan::Context::init_loader failed (no loader?)");
            return;
        }

        auto context = std::make_unique<Vulkan::Context>();
        // 4 thread indices: the EE thread (GIF/vsync path) and the host
        // thread (presentation path) both drive this backend through the
        // adapter mutex; distinct Granite per-thread command pools keep
        // that safe even though calls are serialized.
        context->set_num_thread_indices(4);
        // Push-descriptor ONLY: adding descriptor-heap + descriptor-buffer
        // (the sandbox's flags) segfaults inside lavapipe once a real draw
        // executes; see the file comment. No fallback is attempted because
        // device creation succeeds either way — the crash is asynchronous.
        const unsigned flags = Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT;
        if (!context->init_instance_and_device(nullptr, 0, nullptr, 0, flags))
        {
            setError("ParallelGSBackend: init_instance_and_device failed (no Vulkan device?)");
            return;
        }

        auto device = std::make_unique<Vulkan::Device>();
        device->set_context(*context);
        device->init_frame_contexts(4);

        auto iface = std::make_unique<ParallelGS::GSInterface>();
        ParallelGS::GSOptions options{};
        options.vram_size = vramSize;
        if (!iface->init(device.get(), options))
        {
            setError("ParallelGSBackend: GSInterface::init failed");
            return;
        }

        m_context = std::move(context);
        m_device = std::move(device);
        m_iface = std::move(iface);
        m_deviceReady = true;
    }
    catch (const std::exception &e)
    {
        setError(std::string("ParallelGSBackend::Initialize exception: ") + e.what());
        m_iface.reset();
        m_device.reset();
        m_context.reset();
        m_deviceReady = false;
    }
    catch (...)
    {
        setError("ParallelGSBackend::Initialize unknown exception");
        m_iface.reset();
        m_device.reset();
        m_context.reset();
        m_deviceReady = false;
    }
}

void ParallelGSBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_deviceReady && m_iface)
    {
        try
        {
            m_iface->reset_context_state();
        }
        catch (...)
        {
            setError("ParallelGSBackend::Reset: reset_context_state threw");
        }
    }
    // Backend state is re-initialized; the VRAM allocation (and the GPU
    // VRAM contents) persist, matching the Stage 0 contract.
    m_scanouts.clear();
}

void ParallelGSBackend::GsReset()
{
    // The frontend always calls Reset() first for raw backends; Reset()
    // already performs the parallel-gs context reset, so there is nothing
    // raw-specific left to do here.
}

void ParallelGSBackend::Submit(const GSPrimitiveBatch & /*batch*/)
{
    // Inert by design: decoded delivery is suppressed in raw mode.
}

void ParallelGSBackend::LoadClut(const GSTex0Reg & /*tex0*/, const GSTexClutReg & /*texclut*/)
{
    // Inert by design (see Submit).
}

void ParallelGSBackend::BeginTransfer(const GSTransferCommand & /*command*/)
{
    // Inert by design (see Submit).
}

void ParallelGSBackend::UploadImage(const uint8_t * /*data*/, uint32_t /*sizeBytes*/)
{
    // Inert by design (see Submit).
}

void ParallelGSBackend::TextureFlush()
{
    // Inert by design (see Submit).
}

void ParallelGSBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_deviceReady || !m_iface)
        return;
    try
    {
        m_iface->flush();
    }
    catch (...)
    {
        setError("ParallelGSBackend::Flush: flush threw");
    }
}

void ParallelGSBackend::Sync(GSSyncReason /*reason*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_deviceReady || !m_iface)
        return;
    try
    {
        m_iface->flush();
        refreshMirror();
    }
    catch (...)
    {
        setError("ParallelGSBackend::Sync: flush/refresh threw");
    }
}

void ParallelGSBackend::refreshMirror() const
{
    // Caller holds m_mutex and guarantees m_iface != nullptr.
    if (m_vramMirror.empty())
        return;
    const void *src = m_iface->map_vram_read(0, m_vramMirror.size());
    if (!src)
    {
        // map_vram_read returning null means the page tracker could not
        // produce a host view; keep the previous mirror rather than
        // zeroing it (a stale mirror is diagnosable, a wiped one is not).
        return;
    }
    std::memcpy(m_vramMirror.data(), src, m_vramMirror.size());
    if (m_vramBorrowed && m_vramBorrowedSize > 0u)
    {
        const size_t n = m_vramMirror.size() < m_vramBorrowedSize ? m_vramMirror.size() : m_vramBorrowedSize;
        std::memcpy(m_vramBorrowed, m_vramMirror.data(), n);
    }
}

void ParallelGSBackend::applyPrivRegs(const GSPrivRegsSnapshot &priv)
{
    // Caller holds m_mutex and guarantees m_iface != nullptr.
    ParallelGS::PrivRegisterState &dst = m_iface->get_priv_register_state();
    uint64_t smode1 = priv.smode1;
    if (!videoModeRecognized(smode1, priv.smode2))
    {
        // Log once per distinct unrecognized triple (stderr, like
        // parallel-gs's own [WARN] lines) so the fallback is visible in
        // test and E2 output rather than silent.
        const uint64_t key = ((smode1 >> 3) & 0x7Full) | (((smode1 >> 13) & 0x3ull) << 8) |
                             ((priv.smode2 & 1u) << 10);
        if (key != m_lastFallbackLogged)
        {
            m_lastFallbackLogged = key;
            std::fprintf(stderr,
                         "[ParallelGSBackend] unrecognized video mode (CMOD=%llu, LC=%llu, INT=%llu); "
                         "falling back to %s for scanout\n",
                         static_cast<unsigned long long>((smode1 >> 13) & 0x3ull),
                         static_cast<unsigned long long>((smode1 >> 3) & 0x7Full),
                         static_cast<unsigned long long>(priv.smode2 & 1u),
                         (priv.smode2 & 1u) ? "CMOD=NTSC, LC=analog" : "LC=analog");
        }
        smode1 = fallbackSmode1(smode1, priv.smode2);
    }
    assignRegBits(dst.pmode, priv.pmode);
    assignRegBits(dst.smode1, smode1);
    assignRegBits(dst.smode2, priv.smode2);
    assignRegBits(dst.srfsh, priv.srfsh);
    assignRegBits(dst.synch1, priv.synch1);
    assignRegBits(dst.synch2, priv.synch2);
    assignRegBits(dst.syncv, priv.syncv);
    assignRegBits(dst.dispfb1, priv.dispfb1);
    assignRegBits(dst.display1, priv.display1);
    assignRegBits(dst.dispfb2, priv.dispfb2);
    assignRegBits(dst.display2, priv.display2);
    assignRegBits(dst.extbuf, priv.extbuf);
    assignRegBits(dst.extdata, priv.extdata);
    assignRegBits(dst.extwrite, priv.extwrite);
    assignRegBits(dst.bgcolor, priv.bgcolor);
    assignRegBits(dst.csr, priv.csr);
    assignRegBits(dst.imr, priv.imr);
    assignRegBits(dst.busdir, priv.busdir);
    assignRegBits(dst.siglblid, priv.siglblid);
}

std::vector<uint8_t> ParallelGSBackend::runScanoutAndReadback(const GSPrivRegsSnapshot &priv, uint32_t field,
                                                             uint32_t &outWidth, uint32_t &outHeight,
                                                             bool &outInterlaced, uint32_t &outPhase)
{
    // Caller holds m_mutex and guarantees m_deviceReady && m_iface.
    applyPrivRegs(priv);

    ParallelGS::VSyncInfo info{};
    info.phase = field;
    // Repro-tool readback layout: the scanout image lands in
    // READ_ONLY_OPTIMAL and is barriered to TRANSFER_SRC below.
    info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    info.force_progressive = false;
    info.overscan = false;
    info.anti_blur = false;
    info.crtc_offsets = false;
    // v1 presentation is host-RGBA frames for comparison against the CPU
    // backend (Stage 0 contract), not analog TV output: do NOT adapt to
    // the internal horizontal resolution. With adapt=true, parallel-gs
    // scales the mode width by clock_divider / (MAGH+1) (2/4 = 0.5 for
    // this content), producing a 320-wide downsampled frame that cannot
    // be compared against the CPU backend's 640-wide DISPLAY-resolution
    // frame. With adapt=false the merged scanout image is mode_width x
    // mode_height (640x448 here), sampling the framebuffer 1:1. Analog
    // downsampling is a display-fidelity choice for a future
    // native-display presentation, not for frame comparison.
    info.adapt_to_internal_horizontal_resolution = false;
    info.raw_circuit_scanout = false;
    info.high_resolution_scanout = false;
    info.skip_deinterlace = false;

    ParallelGS::ScanoutResult result = m_iface->vsync(info);
    if (!result.image)
    {
        setError("ParallelGSBackend: vsync returned no scanout image");
        return {};
    }
    if (result.image->get_format() != VK_FORMAT_R8G8B8A8_UNORM)
    {
        setError("ParallelGSBackend: unexpected scanout pixel format (expected R8G8B8A8_UNORM)");
        return {};
    }
    const uint32_t w = result.internal_width;
    const uint32_t h = result.internal_height;
    if (w == 0u || h == 0u)
    {
        setError("ParallelGSBackend: vsync returned zero scanout dimensions");
        return {};
    }

    auto cmd = m_device->request_command_buffer();
    cmd->image_barrier(*result.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    Vulkan::BufferCreateInfo bufInfo{};
    bufInfo.size = static_cast<VkDeviceSize>(w) * h * 4u;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.domain = Vulkan::BufferDomain::CachedHost;
    Vulkan::BufferHandle readback = m_device->create_buffer(bufInfo);
    if (!readback)
    {
        setError("ParallelGSBackend: readback buffer creation failed");
        return {};
    }
    // Tightly packed rows (row_length = slice_height = 0).
    cmd->copy_image_to_buffer(*readback, *result.image, 0, {},
                              { w, h, 1 }, 0, 0,
                              { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    m_device->submit(cmd);
    m_device->wait_idle();

    void *mapped = m_device->map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT);
    if (!mapped)
    {
        setError("ParallelGSBackend: readback buffer map failed");
        return {};
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
    std::memcpy(pixels.data(), mapped, pixels.size());
    m_device->unmap_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT);

    outWidth = w;
    outHeight = h;
    outInterlaced = result.interlaced;
    outPhase = result.interlace_phase;
    return pixels;
}

PresentationFrame ParallelGSBackend::Present(const GSPresentationRequest &request)
{
    // v1 presentation contract: decode the synced mirror 1:1, exactly as
    // the CPU backend decodes its VRAM (same display-size, DISPFB,
    // PMODE/SMODE2, preferred-source, black-frame fallback, field, and
    // dual-CRT blend rules). Present() does NOT run a GPU scanout: the
    // scanout image applies CRTC magnification (MAGH/DW sampling) that
    // the CPU present does not, so scanout pixels cannot be compared
    // against CPU goldens (see the file comment for the measured MAGH
    // evidence). The scanout still runs at VSync() for field observation.
    // Contract note: Present() reads the last synced mirror; it does not
    // force a fresh GPU readback. The frontend's latch path always calls
    // Flush() + Sync(Presentation) before Present(), so the mirror is
    // current there; direct Present() callers must Sync() first.
    std::lock_guard<std::mutex> lock(m_mutex);
    PresentationFrame frame{};
    if (!m_deviceReady || !m_iface || m_vramMirror.empty())
    {
        if (m_lastError.empty())
            setError("ParallelGSBackend::Present: device not ready");
        return frame;
    }

    try
    {
        const bool enableCrt1 = (request.pmode & 0x1ull) != 0ull;
        const bool enableCrt2 = (request.pmode & 0x2ull) != 0ull;
        const bool mmod = ((request.pmode >> 5) & 0x1ull) != 0ull;
        const bool amod = ((request.pmode >> 6) & 0x1ull) != 0ull;
        const bool slbg = ((request.pmode >> 7) & 0x1ull) != 0ull;
        const uint8_t alp = static_cast<uint8_t>((request.pmode >> 8) & 0xFFu);
        const bool interlaced = (request.smode2 & 0x1ull) != 0ull;
        const bool frameMode = ((request.smode2 >> 1) & 0x1ull) != 0ull;
        const bool fieldMode = interlaced && !frameMode;
        const bool oddField = (request.vsyncTick & 1ull) != 0ull;

        const PresentFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
        const PresentFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
        const PresentReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
        const PresentReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
        uint32_t width1 = 0, height1 = 0, width2 = 0, height2 = 0;
        decodeDisplaySize(request.display1, width1, height1);
        decodeDisplaySize(request.display2, width2, height2);
        const bool valid1 = enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
        const bool valid2 = enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
        if (!valid1 && !valid2)
            return frame;

        const uint8_t *mirror = m_vramMirror.data();
        // copySource port: preferred source, then the display frame, then
        // the black-frame context fallback (only when fbp == 0).
        auto copySource = [&](const PresentFrameReg &displayFrame,
                              const PresentReadOrigin &origin,
                              uint32_t width, uint32_t height,
                              bool allowPreferred, bool preserveAlpha,
                              PresentFrameReg &selected,
                              std::vector<uint8_t> &pixels,
                              bool &usedPreferred) -> bool
        {
            selected = displayFrame;
            pixels.clear();
            usedPreferred = false;
            if (allowPreferred && request.hasPreferredSource &&
                request.preferredDestFbp == displayFrame.fbp &&
                (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp))
            {
                PresentFrameReg preferred{};
                preferred.fbp = request.preferredSource.fbp;
                preferred.fbw = request.preferredSource.fbw;
                preferred.psm = request.preferredSource.psm;
                if (copyFrameToHostRgba(mirror, preferred, width, height, pixels, preserveAlpha, 0u, 0u))
                {
                    selected = preferred;
                    usedPreferred = true;
                }
            }
            if (pixels.empty() &&
                !copyFrameToHostRgba(mirror, displayFrame, width, height, pixels, preserveAlpha, origin.x, origin.y))
                return false;
            if (!usedPreferred && displayFrame.fbp == 0u &&
                countNonBlackPixels(pixels, width, height) == 0u)
            {
                for (const GSFrameReg &candidate : request.contextFrames)
                {
                    if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw &&
                        candidate.psm == selected.psm)
                        continue;
                    PresentFrameReg candidateFrame{};
                    candidateFrame.fbp = candidate.fbp;
                    candidateFrame.fbw = candidate.fbw;
                    candidateFrame.psm = candidate.psm;
                    std::vector<uint8_t> candidatePixels;
                    if (!copyFrameToHostRgba(mirror, candidateFrame, width, height, candidatePixels,
                                             preserveAlpha, 0u, 0u))
                        continue;
                    if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                        continue;
                    selected = candidateFrame;
                    pixels.swap(candidatePixels);
                    break;
                }
            }
            return true;
        };

        if (valid1 && valid2)
        {
            PresentFrameReg selected1{}, selected2{};
            std::vector<uint8_t> crt1, crt2;
            bool preferred1 = false, preferred2 = false;
            if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
                copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
            {
                frame.width = width1 > width2 ? width1 : width2;
                frame.height = height1 > height2 ? height1 : height2;
                frame.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
                const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
                const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
                const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
                for (uint32_t y = 0; y < frame.height; ++y)
                    for (uint32_t x = 0; x < frame.width; ++x)
                    {
                        uint8_t *dst = frame.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                        dst[0] = bgR;
                        dst[1] = bgG;
                        dst[2] = bgB;
                        dst[3] = alp;
                    }
                if (!slbg)
                    for (uint32_t y = 0; y < height2; ++y)
                        std::memcpy(frame.pixels.data() + y * kHostFrameWidth * 4u,
                                    crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
                for (uint32_t y = 0; y < height1; ++y)
                    for (uint32_t x = 0; x < width1; ++x)
                    {
                        const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                        uint8_t *dst = frame.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                        const uint32_t factor = mmod ? alp
                                                     : (static_cast<uint32_t>(src[3]) * 2u < 255u
                                                            ? static_cast<uint32_t>(src[3]) * 2u : 255u);
                        dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                        dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                        dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                        dst[3] = amod ? dst[3] : src[3];
                    }
                normalizePresentationAlpha(frame.pixels, frame.width, frame.height);
                if (fieldMode)
                    applyFieldPresentation(frame.pixels, frame.width, frame.height, oddField);
                frame.displayFbp = displayFrame1.fbp;
                frame.sourceFbp = selected1.fbp;
                return frame;
            }
        }

        const PresentFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
        const PresentReadOrigin &origin = valid1 ? origin1 : origin2;
        frame.width = valid1 ? width1 : width2;
        frame.height = valid1 ? height1 : height2;
        PresentFrameReg selected = displayFrame;
        if (!copySource(displayFrame, origin, frame.width, frame.height, true, false, selected, frame.pixels,
                        frame.usedPreferred))
            return {};
        if (fieldMode)
            applyFieldPresentation(frame.pixels, frame.width, frame.height, oddField);
        normalizePresentationAlpha(frame.pixels, frame.width, frame.height);
        frame.displayFbp = displayFrame.fbp;
        frame.sourceFbp = selected.fbp;
        return frame;
    }
    catch (const std::exception &e)
    {
        setError(std::string("ParallelGSBackend::Present exception: ") + e.what());
        return PresentationFrame{};
    }
    catch (...)
    {
        setError("ParallelGSBackend::Present unknown exception");
        return PresentationFrame{};
    }
}

void ParallelGSBackend::VSync(const GSPrivRegsSnapshot &privRegs, uint32_t field)
{
    // Runs the GPU scanout with the full priv snapshot and records the
    // field observation (E2d). The scanout pixels themselves are NOT kept
    // for presentation in v1 (Present() decodes the mirror 1:1 instead);
    // VSync() exists here to observe interlaced/phase behaviour and to
    // keep the GPU scanout path exercised at vsync rate.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_deviceReady || !m_iface)
        return;
    try
    {
        uint32_t w = 0, h = 0, phase = 0;
        bool interlaced = false;
        std::vector<uint8_t> pixels = runScanoutAndReadback(privRegs, field, w, h, interlaced, phase);
        if (pixels.empty())
            return;

        ScanoutObservation obs{};
        obs.vsyncTick = privRegs.vsyncTick;
        obs.field = field;
        obs.interlaced = interlaced;
        obs.interlacePhase = phase;
        obs.width = w;
        obs.height = h;
        m_scanouts.push_back(obs);
        if (!m_scanoutLogPath.empty())
        {
            std::ofstream log(m_scanoutLogPath, std::ios::app);
            if (log)
            {
                log << "{\"tick\":" << obs.vsyncTick << ",\"field\":" << obs.field
                    << ",\"interlaced\":" << (obs.interlaced ? 1 : 0)
                    << ",\"phase\":" << obs.interlacePhase
                    << ",\"w\":" << obs.width << ",\"h\":" << obs.height << "}\n";
            }
        }
    }
    catch (...)
    {
        setError("ParallelGSBackend::VSync: scanout threw");
    }
}

void ParallelGSBackend::RawGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_deviceReady || !m_iface)
        return;
    if (!data || sizeBytes == 0u)
        return;
    // parallel-gs asserts path_index < 4 and size % 16 == 0; a backend
    // must never let a malformed guest stream crash the process, so
    // validate here and report through LastError() instead.
    if (path < 1u || path > 3u)
    {
        setError("ParallelGSBackend::RawGif: path out of range (expected 1..3)");
        return;
    }
    if ((sizeBytes % 16u) != 0u)
    {
        setError("ParallelGSBackend::RawGif: packet size not a multiple of 16 bytes");
        return;
    }
    try
    {
        m_iface->gif_transfer(path, data, static_cast<size_t>(sizeBytes));
    }
    catch (...)
    {
        setError("ParallelGSBackend::RawGif: gif_transfer threw");
        return;
    }
}

bool ParallelGSBackend::ClearFramebuffer(const GSContext & /*context*/, uint32_t /*rgba*/)
{
    // Not implemented for v1: a GPU clear would need a swizzled VRAM
    // write through the page tracker plus a scanout-visible flush, and no
    // E2 fixture exercises this debug helper. Returning false is the
    // honest signal; callers already handle it.
    return false;
}

uint32_t ParallelGSBackend::ConsumeLocalToHostBytes(uint8_t * /*dst*/, uint32_t /*maxBytes*/)
{
    // Not implemented for v1: local-to-host transfers through the adapter
    // are not wired yet. No E2 fixture uses them.
    return 0;
}

uint32_t ParallelGSBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vramMirror.empty())
        return 0;
    // Contract: after Sync(), prior work is visible. ReadVram reads the
    // last synced mirror; it does not force a fresh GPU readback.
    return readPixel(psm, m_vramMirror.data(), base, bw, x, y);
}

void ParallelGSBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vramMirror.empty())
        return;
    writePixel(psm, m_vramMirror.data(), base, bw, x, y, value);
    if (!m_deviceReady || !m_iface)
        return;
    // Push the whole mirror back so the GPU and the mirror agree. Full
    // 4 MiB push per pixel is expensive, but WriteVram is a debug/test
    // path, never a frame path; correctness over speed here.
    try
    {
        void *dst = m_iface->map_vram_write(0, m_vramMirror.size());
        if (!dst)
            return;
        std::memcpy(dst, m_vramMirror.data(), m_vramMirror.size());
        m_iface->end_vram_write(0, m_vramMirror.size());
    }
    catch (...)
    {
        setError("ParallelGSBackend::WriteVram: mirror push threw");
    }
}

void ParallelGSBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_deviceReady && m_iface)
    {
        try
        {
            refreshMirror();
        }
        catch (...)
        {
            // Fall through with the last good mirror; LastError is set
            // only for the non-const paths to avoid surprising const
            // callers — the mirror itself is the evidence.
        }
    }
    out = m_vramMirror;
}

GSTransferSnapshot ParallelGSBackend::GetTransferSnapshot() const
{
    // Raw backends carry no frontend-visible transfer state; the default
    // (direction 3, zero counts) is the honest report.
    return GSTransferSnapshot{};
}
