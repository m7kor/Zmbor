#include "Common.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_set>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        // IOP side of the SIF command protocol, as implemented by the IOP SIFCMD/SIFRPC modules (ps2sdk
        // iop/system/sifcmd). EE code that is recompiled rather than replaced by named stubs (stripped ELFs) reaches
        // the IOP only through these packets: sceSifSetDma to the command buffer the IOP publishes in SIF_REG_SUBADDR,
        // answered by packets written to the EE receive buffer, each followed by a DMAC SIF0 interrupt.
        struct IopToEePacket
        {
            std::vector<uint8_t> packet;
            uint32_t extraDest = 0u;
            std::vector<uint8_t> extra;
        };

        struct IopRpcServer
        {
            uint32_t sid = 0u;
            uint32_t buffer = 0u;
        };

        std::deque<IopToEePacket> g_iopOutbound;
        bool g_iopDeliveryInFlight = false;
        uint32_t g_eeCmdReceiveBuffer = 0u;
        uint32_t g_iopCmdBuffer = 0u;
        std::unordered_map<uint32_t, IopRpcServer> g_iopRpcServers; // keyed by the server descriptor address
        std::unordered_map<uint32_t, uint32_t> g_iopRpcServerBySid;

        constexpr uint32_t kSifRegIopCmdBuffer = 0x2u; // SIF_REG_SUBADDR
        constexpr uint32_t kSifCmdChangeSaddr = 0x80000000u;
        constexpr uint32_t kSifCmdSetSreg = 0x80000001u;
        constexpr uint32_t kSifCmdInitCmd = 0x80000002u;
        constexpr uint32_t kSifCmdRpcEnd = 0x80000008u;
        constexpr uint32_t kSifCmdRpcBind = 0x80000009u;
        constexpr uint32_t kSifCmdRpcCall = 0x8000000Au;
        constexpr uint32_t kSifCmdRpcRdata = 0x8000000Cu;
        constexpr uint32_t kSifRpcPacketSize = 64u;
        constexpr uint32_t kIopCmdBufferSize = 0x200u;
        constexpr uint32_t kIopRpcServerDescriptorSize = 0x80u;
        constexpr uint32_t kIopRpcServerBufferSize = 0x10000u;
        constexpr uint32_t kDmacSif0 = 5u;
        constexpr uint32_t kFileIoSid = 0x80000001u; // ROM FILEIO server
        constexpr uint32_t kFileIoWrite = 3u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;
            g_iopOutbound.clear();
            g_iopDeliveryInFlight = false;
            g_eeCmdReceiveBuffer = 0u;
            g_iopCmdBuffer = 0u;
            g_iopRpcServers.clear();
            g_iopRpcServerBySid.clear();

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }

        uint32_t packetWord(const std::vector<uint8_t> &packet, size_t offset)
        {
            uint32_t value = 0u;
            if (offset + sizeof(value) <= packet.size())
                std::memcpy(&value, packet.data() + offset, sizeof(value));
            return value;
        }

        void setPacketWord(std::vector<uint8_t> &packet, size_t offset, uint32_t value)
        {
            if (packet.size() < offset + sizeof(value))
                packet.resize(offset + sizeof(value), 0u);
            std::memcpy(packet.data() + offset, &value, sizeof(value));
        }

        // Header: psize:8 dsize:24, dest, cid, opt.
        void queueIopToEeLocked(std::vector<uint8_t> packet, uint32_t cid, uint32_t extraDest, std::vector<uint8_t> extra)
        {
            const uint32_t extraSize = extraDest != 0u ? static_cast<uint32_t>(extra.size()) : 0u;
            setPacketWord(packet, 0u, (static_cast<uint32_t>(packet.size()) & 0xFFu) | (extraSize << 8));
            setPacketWord(packet, 4u, extraSize != 0u ? extraDest : 0u);
            setPacketWord(packet, 8u, cid);
            g_iopOutbound.push_back(IopToEePacket{std::move(packet), extraDest, extraSize != 0u ? std::move(extra) : std::vector<uint8_t>{}});
        }

        // The EE has one receive slot (its handler copies the packet out and clears psize), so packets go one at a
        // time: the next is written only after the SIF0 handler for the previous one has returned.
        void deliverIopPackets(uint8_t *rdram, PS2Runtime *runtime)
        {
            IopToEePacket next;
            uint32_t receiveBuffer = 0u;
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                if (g_iopDeliveryInFlight || g_iopOutbound.empty() || g_eeCmdReceiveBuffer == 0u)
                    return;
                next = std::move(g_iopOutbound.front());
                g_iopOutbound.pop_front();
                g_iopDeliveryInFlight = true;
                receiveBuffer = g_eeCmdReceiveBuffer;
            }
            if (!next.extra.empty())
                (void)writeEeRange(rdram, next.extraDest, next.extra.data(), static_cast<uint32_t>(next.extra.size()));
            (void)writeEeRange(rdram, receiveBuffer, next.packet.data(), static_cast<uint32_t>(next.packet.size()));
            const bool dispatched = runtime->eeScheduler().dispatchIrq(true, kDmacSif0, [rdram, runtime]()
                                                                       {
                {
                    std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                    g_iopDeliveryInFlight = false;
                }
                deliverIopPackets(rdram, runtime); });
            if (!dispatched)
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                g_iopDeliveryInFlight = false;
            }
        }

        uint32_t iopRpcServerForSidLocked(PS2Runtime *runtime, uint32_t sid)
        {
            const auto existing = g_iopRpcServerBySid.find(sid);
            if (existing != g_iopRpcServerBySid.end())
                return existing->second;
            const uint32_t descriptor = runtime->allocateIopMemory(kIopRpcServerDescriptorSize, 64u);
            const uint32_t buffer = runtime->allocateIopMemory(kIopRpcServerBufferSize, 64u);
            if (descriptor == 0u || buffer == 0u)
                return 0u;
            g_iopRpcServers[descriptor] = IopRpcServer{sid, buffer};
            g_iopRpcServerBySid[sid] = descriptor;
            return descriptor;
        }

        void logIopProtocolOnce(const char *what, uint32_t a, uint32_t b)
        {
            static std::mutex logMutex;
            static std::unordered_set<uint64_t> logged;
            std::lock_guard<std::mutex> lock(logMutex);
            if (logged.size() < 64u && logged.insert((static_cast<uint64_t>(a) << 32) | b).second)
                std::cerr << "[SIF/IOP] " << what << " 0x" << std::hex << a << " 0x" << b << std::dec << std::endl;
        }

        // FILEIO write (ROM FILEIO protocol, as sent by ps2sdk's fioWrite): {fd, ptr, size, mis, aligned[16]}; the
        // first mis bytes travel in the packet because the IOP DMAs only the 16-byte aligned rest from EE memory.
        // fd 1 and 2 are the IOP's tty, which this host shows as stdout and stderr.
        bool serveFileIoWrite(const uint8_t *rdram, uint32_t sendAddress, uint32_t sendSize, int32_t &result)
        {
            uint32_t argument[8] = {};
            if (sendSize < 32u || !readEeRange(rdram, sendAddress, argument, sizeof(argument)))
                return false;
            const int32_t fd = static_cast<int32_t>(argument[0]);
            const uint32_t pointer = argument[1];
            const int32_t size = static_cast<int32_t>(argument[2]);
            const uint32_t misaligned = std::min<uint32_t>(argument[3], 16u);
            if ((fd != 1 && fd != 2) || size < 0 || static_cast<uint32_t>(size) > PS2_RAM_SIZE || misaligned > static_cast<uint32_t>(size))
                return false;
            std::vector<uint8_t> text(static_cast<size_t>(size));
            std::memcpy(text.data(), reinterpret_cast<const uint8_t *>(argument) + 16u, misaligned);
            if (!readEeRange(rdram, pointer + misaligned, text.data() + misaligned, static_cast<uint32_t>(size) - misaligned))
                return false;
            FILE *stream = fd == 1 ? stdout : stderr;
            std::fwrite(text.data(), 1u, text.size(), stream);
            std::fflush(stream);
            // PS2X_TTY_LOG=<file> also collects the guest's tty output alone, apart from the runtime's own logging.
            static std::mutex ttyLogMutex;
            static FILE *ttyLog = []() -> FILE *
            {
                const char *path = std::getenv("PS2X_TTY_LOG");
                return path && *path ? std::fopen(path, "wb") : nullptr;
            }();
            if (ttyLog)
            {
                std::lock_guard<std::mutex> lock(ttyLogMutex);
                std::fwrite(text.data(), 1u, text.size(), ttyLog);
                std::fflush(ttyLog);
            }
            result = size;
            return true;
        }

        std::vector<uint8_t> serveIopRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t sid,
                                         uint32_t function, uint32_t sendAddress, uint32_t sendSize,
                                         uint32_t receiveAddress, uint32_t receiveSize)
        {
            std::vector<uint8_t> result(receiveSize, 0u);
            if (sid == kFileIoSid && function == kFileIoWrite)
            {
                int32_t written = -1;
                if (serveFileIoWrite(rdram, sendAddress, sendSize, written) && receiveSize >= 4u)
                {
                    std::memcpy(result.data(), &written, sizeof(written));
                    return result;
                }
            }

            ps2x::iop::RpcRequest request{};
            request.sid = sid;
            request.function = function;
            request.send = {sendAddress, sendSize};
            request.receive = {receiveAddress, receiveSize};
            const ps2x::iop::RpcResult handled = PS2IopTransport::handleRpc(runtime, rdram, ctx, request);
            if (!handled.handled)
            {
                logIopProtocolOnce("unhandled RPC sid/function", sid, function);
                return result;
            }
            const uint32_t source = handled.resultAddress != 0u ? handled.resultAddress : receiveAddress;
            if (source != 0u && receiveSize != 0u)
                (void)readEeRange(rdram, source, result.data(), receiveSize);
            return result;
        }

        // One EE-to-IOP command packet; extraSource is the EE address of the data sent with it (0 when none).
        void handleIopCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                              const std::vector<uint8_t> &packet, uint32_t extraSource)
        {
            const uint32_t cid = packetWord(packet, 8u);
            const auto rpcEnd = [&](uint32_t client, uint32_t requestCid)
            {
                std::vector<uint8_t> rend(kSifRpcPacketSize, 0u);
                setPacketWord(rend, 16u, packetWord(packet, 16u)); // rec_id
                setPacketWord(rend, 20u, packetWord(packet, 20u)); // pkt_addr
                setPacketWord(rend, 24u, packetWord(packet, 24u)); // rpc_id
                setPacketWord(rend, 28u, client);
                setPacketWord(rend, 32u, requestCid);
                return rend;
            };

            switch (cid)
            {
            case kSifCmdChangeSaddr:
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                g_eeCmdReceiveBuffer = packetWord(packet, 16u);
                break;
            }
            case kSifCmdInitCmd:
            {
                // opt 0 carries the EE receive buffer; the IOP's SIFRPC then reports itself ready with
                // SET_SREG(SIF_SREG_RPCINIT, 1), which the EE's sceSifInitRpc waits for. opt 1 needs no answer.
                if (packetWord(packet, 12u) != 0u)
                    break;
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                g_eeCmdReceiveBuffer = packetWord(packet, 16u);
                std::vector<uint8_t> setSreg(24u, 0u);
                setPacketWord(setSreg, 16u, 0u); // SIF_SREG_RPCINIT
                setPacketWord(setSreg, 20u, 1u);
                queueIopToEeLocked(std::move(setSreg), kSifCmdSetSreg, 0u, {});
                break;
            }
            case kSifCmdSetSreg:
            case kSifCmdRpcEnd:
                break; // IOP soft registers and answers to IOP-initiated calls: nothing on this side reads them.
            case kSifCmdRpcBind:
            {
                const uint32_t sid = packetWord(packet, 32u);
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                // FILEIO is resident in the IOP ROM and served here; other servers exist once the IOP side has them.
                const bool bindable = sid == kFileIoSid || PS2IopTransport::canBindRpc(runtime, sid);
                const uint32_t server = bindable ? iopRpcServerForSidLocked(runtime, sid) : 0u;
                std::vector<uint8_t> rend = rpcEnd(packetWord(packet, 28u), kSifCmdRpcBind);
                setPacketWord(rend, 36u, server);
                setPacketWord(rend, 40u, server != 0u ? g_iopRpcServers[server].buffer : 0u);
                queueIopToEeLocked(std::move(rend), kSifCmdRpcEnd, 0u, {});
                break;
            }
            case kSifCmdRpcCall:
            {
                const uint32_t client = packetWord(packet, 28u);
                const uint32_t function = packetWord(packet, 32u);
                const uint32_t sendSize = packetWord(packet, 36u);
                const uint32_t receiveAddress = packetWord(packet, 40u);
                const uint32_t receiveSize = std::min<uint32_t>(packetWord(packet, 44u), PS2_RAM_SIZE);
                const uint32_t replyMode = packetWord(packet, 48u);
                uint32_t sid = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                    const auto server = g_iopRpcServers.find(packetWord(packet, 52u));
                    if (server != g_iopRpcServers.end())
                        sid = server->second.sid;
                }
                if (sid == 0u)
                    logIopProtocolOnce("RPC call to unknown server/function", packetWord(packet, 52u), function);
                std::vector<uint8_t> result = serveIopRpc(rdram, ctx, runtime, sid, function, extraSource,
                                                          extraSource != 0u ? sendSize : 0u, receiveAddress, receiveSize);
                if (replyMode != 0u)
                {
                    std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                    queueIopToEeLocked(rpcEnd(client, kSifCmdRpcCall), kSifCmdRpcEnd,
                                       receiveSize != 0u ? receiveAddress : 0u, std::move(result));
                }
                else if (receiveSize != 0u && receiveAddress != 0u)
                {
                    (void)writeEeRange(rdram, receiveAddress, result.data(), receiveSize);
                }
                else
                {
                    // No reply packet: the IOP frees the caller's packet by overwriting it with a cleared one.
                    std::vector<uint8_t> cleared = rpcEnd(client, kSifCmdRpcCall);
                    setPacketWord(cleared, 16u, 0u);
                    setPacketWord(cleared, 24u, 0u);
                    (void)writeEeRange(rdram, packetWord(packet, 20u), cleared.data(), kSifRpcPacketSize);
                }
                break;
            }
            case kSifCmdRpcRdata:
            {
                const uint32_t source = packetWord(packet, 32u);
                const uint32_t dest = packetWord(packet, 36u);
                const uint32_t size = std::min<uint32_t>(packetWord(packet, 40u), PS2_RAM_SIZE);
                std::vector<uint8_t> data(size, 0u);
                if (size != 0u && !runtime->readIopMemory(source, data.data(), data.size()))
                    logIopProtocolOnce("RPC rdata from invalid IOP range", source, size);
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                queueIopToEeLocked(rpcEnd(packetWord(packet, 28u), kSifCmdRpcRdata), kSifCmdRpcEnd, dest, std::move(data));
                break;
            }
            default:
                logIopProtocolOnce("unhandled SIF command", cid, packetWord(packet, 12u));
                break;
            }
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->runtimeMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->runtimeFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->runtimeFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->runtimeFree(packetAddress);
            return false;
        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        if (reg == kSifRegIopCmdBuffer && runtime)
        {
            // The IOP's SIFCMD publishes its command receive buffer here once it is running.
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            if (g_iopCmdBuffer == 0u)
                g_iopCmdBuffer = runtime->allocateIopMemory(kIopCmdBufferSize, 64u);
            g_sifRegs[reg] = g_iopCmdBuffer;
        }
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        uint32_t iopCmdBuffer = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            iopCmdBuffer = g_iopCmdBuffer;
        }
        for (uint32_t i = 0; iopCmdBuffer != 0u && i < pendingCount; ++i)
        {
            const Ps2SifDmaTransfer &xfer = pending[i];
            if (xfer.dest != iopCmdBuffer || xfer.size < 16)
                continue;
            std::vector<uint8_t> packet(static_cast<size_t>(xfer.size));
            if (!readEeRange(rdram, xfer.src, packet.data(), static_cast<uint32_t>(packet.size())))
                continue;
            // sceSifSendCmd queues the packet's extra data as the transfer to header.dest just before it.
            uint32_t extraSource = 0u;
            const uint32_t extraDest = packetWord(packet, 4u);
            for (uint32_t j = 0; extraDest != 0u && j < i; ++j)
            {
                if (pending[j].dest == extraDest)
                    extraSource = pending[j].src;
            }
            handleIopCommand(rdram, ctx, runtime, packet, extraSource);
        }
        if (runtime)
            deliverIopPackets(rdram, runtime);

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
