# PS2 timing architecture

How guest time advances today, measured from the code at the head of this branch, and what is not modelled. Timing
claims use four levels: functional (order only), approximate, hardware-calibrated, cycle-accurate. No component is
hardware-calibrated or cycle-accurate yet.

## Current model

- **EE cycles.** Recompiled code reports cycles at checkpoints; `EeScheduler::accountCycles` adds them to the guest
  cycle counter (`kEeClockHz`), advances the EE timers (`PS2Memory::advanceEeTimers`) and the IOP
  (`PS2Runtime::advanceIopEeCycles`) by the same amount. The per-instruction cycle cost is the recompiler's estimate,
  not a pipeline model: functional/approximate.
- **Scheduled events** (VBlank start/end, alarms, timer interrupts) carry a guest deadline (`deadlineCycle`) and a
  host deadline. `EeScheduler::processDueDeadlines` fires an event only when the guest counter has reached its
  deadline; the host deadline only delays it (pacing to real time while the guest waits). Guest-visible order is
  therefore decided by guest cycles at checkpoint granularity, not by host time. VBlank is every 16,667 µs of guest
  time (NTSC field), approximate.
- **DMA** completes synchronously when started (`dma.timing` is `HLE_OR_STUB`): a transfer's data is delivered before
  the guest's next instruction. Exceptions that keep a channel active: VIF stalls (I bit), and the MFIFO drain waiting
  for fromSPR (this branch).
- **VU microprograms** run when MSCAL/VCALLMS start them; VU1 has a cycle-level pipeline model (lockstep-tested
  against a reference), but VU execution is not interleaved with EE cycles.
- **GS** work is processed when packets arrive; there is no GS timing.
- **IOP** advances from EE cycles through the runtime; its kernel and devices are HLE.

## Not modelled
Bus arbitration and DMA cycle cost, cache timing, EE pipeline stalls, VU/EE concurrency, GS fill rate, SPU2 sample
clock, CDVD seek/read latency. Each is a separate component in the inventory with its own status.

## Target (wave 1)
One guest-time event queue owned by the scheduler, with every device scheduling completions in guest cycles; host
time only for pacing and presentation; deterministic replay tests that run the same workload twice and compare event
order and state.
