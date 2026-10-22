# ADR 0001: one raw-GIF seam for whole-GS renderers

Status: proposed (design only; nothing in this ADR is implemented yet).

## Context

Upstream's `GSRasterBackend` receives decoded work from the GS front end: primitive batches, CLUT loads, image
transfers, presentation requests and readbacks. Four community projects needed to plug in a renderer that wants the
raw GS input instead (paraLLEl-GS in BT3 and Def Jam Vendetta, PCSX2's GS in Killzone and Urban Reign), and each
built its own seam:

- BT3: free functions hooked at the GIF arbiter, beside its own GL renderer.
- Killzone: a `PS2HostGs` function-pointer struct (`gifPacket`, `vsync`, `exclusive`).
- Urban Reign: four defaulted virtuals on `GSRasterBackend` (`WantsRawGif`, `RawGif`, `VSync`, `GsReset`).
- Def Jam: a separate `GSGpuRenderer` interface plus a null `GSGpuBackend`, a written ordering contract, a
  triple-buffer presentation mailbox, and a VERIFY mode that compares GS memory pages between the CPU and GPU renderers.

All four keep SIGNAL/FINISH/LABEL/CSR handling in the PS2Recomp front end, because the guest reads them.

## Decision

Adopt Urban Reign's minimal, backward-compatible shape and Def Jam's contract:

1. `GSRasterBackend` gains defaulted virtuals: `bool WantsRawGif()`, `RawGif(path, data, bytes)`,
   `VSync(const GSPrivRegs&, field)`, `GsReset()`. When `WantsRawGif()` is true the front end still parses tags and
   privileged-register writes but stops sending draws, CLUT loads and image data.
2. The GIF arbiter callback carries the PATH index (today it is `void(const uint8_t*, uint32_t)`).
3. The privileged register block is captured in stream order at each vsync (BT3 found that live reads mismatch flips).
4. Document the ordering contract: one producer; `ReadVram`, `SnapshotVram`, `ConsumeLocalToHost` and
   `Sync(LocalToHost)` observe every earlier call.
5. A comparison mode in the spirit of Def Jam's VERIFY becomes the GS regression oracle: run the CPU reference renderer
   and a second backend on the same stream and diff GS memory page hashes.

## Consequences

- paraLLEl-GS (LGPL-3.0) or a PCSX2 GS (GPL-3.0) can be added as an optional backend without forking the front end.
- No title-specific code may sit behind the seam (BT3's backend file mixes in game addresses; that part is not taken).
- Each step needs tests: the default path must keep today's golden GS output byte-identical.
