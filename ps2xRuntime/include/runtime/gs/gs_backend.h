#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

// Stage 0 backend contract (GS backend assessment §3.1, ADR 0001). The rules
// below are part of the interface: a backend that violates them is wrong even
// if it renders the golden frame today.
//
// OWNERSHIP / LIFETIME. The frontend owns all guest-visible state (contexts,
// prim/PRMODE, vertex queue, current colour/ST/UV/fog, TEXA/TEXCLUT, transfer
// setup registers, priv-reg pointer, presentation latch) and the VRAM
// allocation itself. The backend only BORROWS the VRAM pointer handed to
// Initialize() and must never free it. Reset() re-initialises backend state
// (CLUT, texture cache, transfer state) but never the VRAM allocation.
// Backend swaps (GS::setRasterBackend) transfer VRAM bytes and replay the
// frontend's full ordered LoadClut history into the fresh backend (Gate E;
// the old last-load-only re-issue lost cumulative, CLD-conditional palette
// state — Gate A S4). History is every decoded LoadClut delivery in order,
// including no-ops, since the last reset; it is never recorded while raw
// mode suppresses delivery, and replay is skipped for raw backends. Replay
// runs the fresh backend's own LoadClut against current VRAM, so palette
// bytes changed after a load without a reload are NOT snapshotted — the
// same VRAM semantics the old re-issue had. Cross-backend snapshot copying
// (reading one backend's palette state into another) is forbidden: replay
// is the only restore mechanism. The texture-page cache self-heals because
// it is a VRAM mirror refilled on demand. Transfer state is NOT preserved
// across a swap: swaps require transfer-idle state (no transfer in
// flight). A swap during an active transfer is out of contract — adapters
// select at startup only, so mid-stream swap is a debug/test operation,
// not a production path.
//
// ORDERING. Single producer (the frontend). After Sync() returns for Finish,
// LocalToHost, Presentation, DebugReadback, or Reset, every prior Submit,
// LoadClut, BeginTransfer, and UploadImage must be visible to ReadVram,
// SnapshotVram, ConsumeLocalToHostBytes, and Present. The CPU backend
// satisfies this trivially (immediate execution); an asynchronous backend
// must fence here. Flush() means "make prior work visible to a subsequent
// Sync()", not "present".
//
// BATCHING. Submit stays per-primitive for v1 (one decoded triangle, sprite,
// line, or point per call, with a full GSDrawState copy). A backend batches
// internally and flushes on Flush()/Sync()/Present(). Begin/end-frame
// markers, if ever needed, arrive as defaulted virtuals — never by changing
// Submit().
//
// PRESENTATION. Presentation stays host-RGBA for v1 (PresentationFrame:
// already-converted RGBA pixels + size + display/source FBP). It is the
// readback path for the debug UI, frame dumps, and check_frame.py.
// Native-texture/swapchain presentation is a later interface extension.
//
// GUEST-VISIBLE STATE NEVER MOVES INTO THE BACKEND. SIGNAL/FINISH/LABEL/CSR,
// PATH3 masking, DIRECTHL/IMAGE ordering, and the GIF arbiter stay in the
// frontend. A backend that moves them is wrong.
//
// ERRORS. Backends report failures through LastError(), never by throwing
// across the GIF path and never silently: the frontend surfaces the string
// through its existing debug/error path. The default (no error) is nullptr.
// Lifetime rule (Gate E §5.2, pinned by test): a recorded failure is
// sticky — a subsequent successful call never clears it; only a backend
// re-Initialize (or a newer failure) replaces it. The frontend latch
// clears (rather than serving a stale frame) when Present returns empty
// after a failure, and the failure string stays readable through
// GS::lastBackendError(). No production code currently polls that channel
// outside tests and tooling; that is an integration gap, not a channel
// defect — the channel itself reports init, submit, and present failures.
class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // Error channel (defaulted, non-breaking). Returns a human-readable
    // description of the backend's last failure, or nullptr when healthy.
    // The string must remain valid until the next backend call.
    virtual const char *LastError() const { return nullptr; }

    // Raw-GIF seam (ADR 0001, Stage 0). All defaulted so the CPU backend and
    // every existing caller are unaffected. A backend that reports
    // WantsRawGif() == true receives whole GIF packets via RawGif() instead
    // of decoded work: the frontend still parses tags and applies
    // privileged-register writes (guest-visible state stays in the
    // frontend) but suppresses Submit, LoadClut, BeginTransfer, UploadImage,
    // and TextureFlush delivery. Raw mode is OFF by default and unreachable
    // by guest code in Stage 0: only an explicitly attached backend can
    // enable it, and adapters select at startup only.
    //
    // path is the GIF path index: 1 (VU1 XGKICK), 2 (VIF1 DIRECT), 3 (GIF
    // DMA). Packets arriving without path information (the legacy
    // unpathed frontend entry, used by tests and direct injection) are
    // reported as path 3.
    virtual bool WantsRawGif() const { return false; }
    virtual void RawGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        (void)path;
        (void)data;
        (void)sizeBytes;
    }
    // Stream-order priv-reg snapshot at a vsync; field alternates with the
    // vsync tick (0/1). Called only while a raw backend is attached.
    virtual void VSync(const GSPrivRegsSnapshot &privRegs, uint32_t field)
    {
        (void)privRegs;
        (void)field;
    }
    // Raw-mode reset. The frontend always calls Reset() first (existing
    // contract) and additionally calls GsReset() when a raw backend is
    // attached, so a raw backend can distinguish a raw-mode reset from a
    // decoded-mode one.
    virtual void GsReset() {}
};
