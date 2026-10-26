// libFuzzer harnesses for the runtime's input-facing paths (docs/architecture/fuzzing.md).
// One translation unit, one executable per target: the target is chosen at compile time with PS2X_FUZZ_TARGET.
//   gif  : GS::processGIFPacket on arbitrary bytes (GIF tags, PACKED/REGLIST/IMAGE data, register writes)
//   vif0 : PS2Memory::processVIF0Data on arbitrary bytes (VIF codes, UNPACK, MPG into VU0 micro memory)
//   vif1 : PS2Memory::processVIF1Data on arbitrary bytes (also DIRECT/DIRECTHL to PATH2, MSCAL)
//   dma  : arbitrary RDRAM contents, then a GIF-channel chain DMA started through the DMAC registers
// State persists across inputs inside one process (as it would across a game's frames); every 4096 inputs the
// memory and GS are rebuilt so that one input's leftovers cannot hide another's defect for long.
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
    struct Machine
    {
        PS2Memory mem;
        GS gs;
        bool ok = false;
        Machine()
        {
            ok = mem.initialize();
            if (ok)
                gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            mem.setGifPacketCallback([this](const uint8_t *packet, uint32_t size) { gs.processGIFPacket(packet, size); });
        }
    };

    std::unique_ptr<Machine> g_machine;
    uint32_t g_inputs = 0;

    Machine &machine()
    {
        if (!g_machine || (++g_inputs % 4096u) == 0u)
            g_machine = std::make_unique<Machine>();
        return *g_machine;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20))
        return -1;
    Machine &m = machine();
    if (!m.ok)
        return 0;
    const uint32_t n = static_cast<uint32_t>(size);
#if PS2X_FUZZ_TARGET == 1 // gif
    m.gs.processGIFPacket(data, n);
#elif PS2X_FUZZ_TARGET == 2 // vif0
    m.mem.processVIF0Data(data, n);
#elif PS2X_FUZZ_TARGET == 3 // vif1
    m.mem.processVIF1Data(data, n);
    m.mem.processPendingTransfers();
#elif PS2X_FUZZ_TARGET == 4 // dma
    // First 4 bytes: tag address (qword aligned, inside the first 1 MB); the rest is copied to RDRAM at 0x100000.
    if (n < 8u)
        return 0;
    uint32_t tadr = 0;
    std::memcpy(&tadr, data, 4);
    tadr = (tadr & 0x000FFFF0u) | 0x00100000u;
    std::memcpy(m.mem.getRDRAM() + 0x100000u, data + 4, std::min<uint32_t>(n - 4u, 0x100000u));
    m.mem.writeIORegister(0x1000A030u, tadr);                // D2_TADR
    m.mem.writeIORegister(0x1000A020u, 0u);                  // D2_QWC
    m.mem.writeIORegister(0x1000A000u, 0x00000105u);         // D2_CHCR: DIR from memory, chain mode, STR
    m.mem.processPendingTransfers();
#else
#error "PS2X_FUZZ_TARGET must be 1 (gif), 2 (vif0), 3 (vif1) or 4 (dma)"
#endif
    return 0;
}
