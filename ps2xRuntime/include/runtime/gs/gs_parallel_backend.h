#pragma once

// paraLLEl-GS Vulkan GS backend adapter (GS Stage 2).
//
// Implements GSRasterBackend behind the Stage 0 raw-GIF seam: whole GIF
// packets arrive via RawGif() and are forwarded to ParallelGS::GSInterface.
// Presentation (v1) is a 1:1 VRAM-derived host-RGBA frame decoded from the
// page-tracker-synced mirror — the same presentation contract as the CPU
// backend (same display-size, field, and CRT-blend rules), so E2 frame
// comparisons measure rendering, not CRTC-model differences. The GPU
// scanout still runs at VSync() for field observation (E2d) and is
// mirrored to the PS2X_PGS_SCANOUT_LOG sidecar, but its CRTC-magnified
// geometry is NOT the presented frame in v1 (see the Present() comment in
// gs_parallel_backend.cpp for the MAGH evidence).
//
// This header intentionally does NOT include Granite/parallel-gs headers:
// the Granite device, context, and GSInterface objects live behind
// unique_ptrs to forward-declared types, so translation units that merely
// name the backend (tests, the gs_verify runner) do not pay for the Vulkan
// headers. Only gs_parallel_backend.cpp includes them.
//
// Build gate: this backend is compiled only when PS2X_ENABLE_PARALLEL_GS is
// ON (see ps2xRuntime/CMakeLists.txt). The macro PS2X_HAS_PARALLEL_GS is 1
// in that configuration and 0 otherwise; including this header in an
// OFF build is a compile error by design (the class would have no
// definition to link against).

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if !PS2X_HAS_PARALLEL_GS
#error "gs_parallel_backend.h requires PS2X_ENABLE_PARALLEL_GS=ON (PS2X_HAS_PARALLEL_GS=1)"
#endif

namespace Vulkan
{
class Context;
class Device;
} // namespace Vulkan

namespace ParallelGS
{
class GSInterface;
} // namespace ParallelGS

class ParallelGSBackend final : public GSRasterBackend
{
public:
    ParallelGSBackend();
    ~ParallelGSBackend() override;

    ParallelGSBackend(const ParallelGSBackend &) = delete;
    ParallelGSBackend &operator=(const ParallelGSBackend &) = delete;

    // GSRasterBackend -------------------------------------------------------
    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    // Decoded-mode delivery is suppressed by the frontend while a raw
    // backend is attached (Stage 0 contract), so these are inert no-ops.
    // They exist only so a misconfigured caller fails silently-safe rather
    // than crashing; they never feed the GPU.
    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void TextureFlush() override;

    void Flush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    // Not implemented for v1 (debug helpers, off the E2 path): returning
    // false / 0 is the honest "not supported" signal, never a faked clear.
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    const char *LastError() const override;

    bool WantsRawGif() const override { return true; }
    void RawGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override;
    void VSync(const GSPrivRegsSnapshot &privRegs, uint32_t field) override;
    void GsReset() override;

    // Test / diagnostic hooks (not part of the backend interface) ----------
    bool deviceReady() const;

    struct ScanoutObservation
    {
        uint64_t vsyncTick = 0;
        uint32_t field = 0;
        bool interlaced = false;
        uint32_t interlacePhase = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    // One entry per VSync() scanout. Also mirrored to the
    // PS2X_PGS_SCANOUT_LOG sidecar file when that environment variable is
    // set at Initialize() time, for the E2d phase-alternation check.
    // (Present() does not run scanouts in v1, so it appends nothing.)
    std::vector<ScanoutObservation> scanoutObservations() const;

private:
    // Runs iface.vsync() with the given priv state and phase, records the
    // observation, and returns the scanout image's host RGBA (empty on
    // failure). Caller holds m_mutex.
    std::vector<uint8_t> runScanoutAndReadback(const GSPrivRegsSnapshot &priv, uint32_t field,
                                               uint32_t &outWidth, uint32_t &outHeight,
                                               bool &outInterlaced, uint32_t &outPhase);

    // Applies a full priv-reg snapshot to the parallel-gs priv state.
    // Caller holds m_mutex and guarantees m_iface != nullptr.
    void applyPrivRegs(const GSPrivRegsSnapshot &priv);

    // Copies the page-tracker-synced GPU VRAM into m_vramMirror and into
    // the borrowed VRAM buffer. Caller holds m_mutex. No-op when the
    // device is not ready.
    void refreshMirror() const;

    void setError(const std::string &message);

    mutable std::mutex m_mutex;
    std::string m_lastError;

    // Borrowed (frontend-owned) VRAM buffer. The adapter never frees it;
    // Sync() mirrors page-tracker-synced GPU VRAM into it so guest VRAM
    // reads and the gs_verify harness's borrowed-buffer capture keep
    // working for adapter runs.
    uint8_t *m_vramBorrowed = nullptr;
    uint32_t m_vramBorrowedSize = 0;

    std::unique_ptr<Vulkan::Context> m_context;
    std::unique_ptr<Vulkan::Device> m_device;
    std::unique_ptr<ParallelGS::GSInterface> m_iface;
    bool m_deviceReady = false;

    // Page-tracker-synced VRAM mirror. Mutable so the const SnapshotVram /
    // ReadVram methods can refresh / read it under the mutex.
    mutable std::vector<uint8_t> m_vramMirror;

    std::vector<ScanoutObservation> m_scanouts;
    std::string m_scanoutLogPath;

    // Last (CMOD, LC) pair for which the video-mode fallback was logged
    // (UINT64_MAX = none yet). Prevents log spam at vsync rate.
    uint64_t m_lastFallbackLogged = UINT64_MAX;
};
