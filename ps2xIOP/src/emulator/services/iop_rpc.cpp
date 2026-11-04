#include <cstdio>
#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_pendingClients.clear();
        m_clientSids.clear();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            // HLE gating (design §7.3, Phase 4 slice): when the
            // DMAC transport owns SIF0, the DMAC engine is the sole
            // owner of IOP->EE transfers and this HLE copy path must
            // not be used for a transfer the engine is running.
            // The flag defaults false, so existing HLE behaviour is
            // unchanged unless slice activation sets it explicitly.
            if (m_memory.transportOwnsSif0())
            {
                setV0(0u);
                return true;
            }
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            // In-flight DmaStat model (design §8), under transport
            // ownership only: 1 = complete, 0 = in-flight, -1 =
            // unknown id (as a 32-bit register value, -1 reads as
            // 0xFFFFFFFF — the same bits as the legacy return, so
            // the int32 interpretation is what distinguishes the
            // unknown case under ownership). Legacy 0xFFFFFFFF is
            // preserved while the transport does not own SIF0.
            if (m_memory.transportOwnsSif0())
                setV0(static_cast<uint32_t>(m_memory.sif0DmaStat(cpu.gpr[4])));
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // sceSifInitRpc
            setV0(0);
            return true;
        // IOP as the client of an EE RPC server (ps2sdk sifcmd exports: 15 sceSifBindRpc, 16 sceSifCallRpc,
        // 18 sceSifCheckStatRpc). SceSifClientData: hdr{pkt_addr, rpc_id, sema_id, mode} 0x00..0x0F, command 0x10,
        // buf 0x14, cbuf 0x18, end_function 0x1C, end_param 0x20, server 0x24. These used to return 0 without
        // binding, so an IRX polling `cd.server` (Crash Twinsanity STREAM.IRX, sid 0x12344321) spun forever.
        case 15: // sceSifBindRpc(cd, sid, mode)
        {
            const uint32_t client = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            ++m_iopClientCalls[(15ull << 32) | sid];
            if (client == 0u || !m_memory.ownsRamRange(client, kClientDataSize))
            {
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            uint32_t server = 0u;
            uint32_t buffer = 0u;
            // An unregistered EE server leaves cd->server at 0: the SDK's documented "retry the bind" signal.
            (void)m_host.findEeRpcServer(sid, server, buffer);
            m_memory.write32(client + 0x0Cu, cpu.gpr[6]); // hdr.mode
            m_memory.write32(client + 0x10u, 0u);
            m_memory.write32(client + 0x14u, buffer);
            m_memory.write32(client + 0x18u, 0u);
            m_memory.write32(client + 0x24u, server);
            if (server != 0u)
                m_clientSids[client] = sid;
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc(cd, fno, mode, send, ssize, recv, rsize, end_func, end_param)
        {
            const uint32_t client = cpu.gpr[4];
            const uint32_t function = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            const uint32_t send = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t sendSize = m_memory.read32(stackPointer + 16u);
            const uint32_t receive = m_memory.read32(stackPointer + 20u);
            const uint32_t receiveSize = m_memory.read32(stackPointer + 24u);
            const uint32_t endFunction = m_memory.read32(stackPointer + 28u);
            const uint32_t endParameter = m_memory.read32(stackPointer + 32u);
            const uint32_t server = client != 0u && m_memory.ownsRamRange(client, kClientDataSize)
                                        ? m_memory.read32(client + 0x24u)
                                        : 0u;
            {
                const auto sid = m_clientSids.find(client);
                ++m_iopClientCalls[(16ull << 32) | (sid != m_clientSids.end() ? sid->second : 0u)];
            }
            if (server == 0u || m_pendingClients.count(client) != 0u ||
                (sendSize != 0u && !m_memory.ownsRamRange(send, sendSize)) ||
                (receiveSize != 0u && !m_memory.ownsRamRange(receive, receiveSize)))
            {
                // Unbound client, a call while the previous one is still running, or a bad buffer.
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            std::vector<uint8_t> payload(sendSize);
            if (sendSize != 0u && !m_memory.readRam(send, payload.data(), payload.size()))
            {
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            m_memory.write32(client + 0x10u, function);
            m_memory.write32(client + 0x1Cu, endFunction);
            m_memory.write32(client + 0x20u, endParameter);
            const bool blocking = (mode & kRpcModeNowait) == 0u;
            const int waiter = blocking ? m_kernel.currentThreadId() : 0;
            const uint32_t gp = cpu.gpr[28];
            m_pendingClients.insert(client);
            const bool queued = m_host.callEeRpc(
                server, function, std::move(payload), receiveSize,
                [this, client, receive, receiveSize, endFunction, endParameter, waiter, gp](const std::vector<uint8_t> &reply)
                {
                    if (receive != 0u && receiveSize != 0u && !reply.empty())
                        (void)m_memory.writeRam(receive, reply.data(), std::min<size_t>(reply.size(), receiveSize));
                    m_pendingClients.erase(client);
                    ++m_iopClientCompletions;
                    if (endFunction != 0u && m_executor != nullptr)
                        (void)m_executor->executeGuestFunction(endFunction, endParameter, 0u, 0u, 0u, gp);
                    if (waiter != 0)
                        (void)m_kernel.wakeupThreadFromHost(waiter);
                });
            if (!queued)
            {
                m_pendingClients.erase(client);
                setV0(static_cast<uint32_t>(-1));
                return true;
            }
            setV0(0);
            // Blocking mode returns once the EE server has answered: sleep until the completion wakes this thread.
            if (blocking && waiter != 0 && m_pendingClients.count(client) != 0u)
                m_kernel.sleepCurrent(cpu);
            return true;
        }
        case 18: // sceSifCheckStatRpc(cd): 1 while a call on this client is in flight
            setV0(m_pendingClients.count(cpu.gpr[4]) != 0u ? 1u : 0u);
            return true;
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        const bool served = serverIt != m_servers.end() && serverIt->second.function != 0u;
        {
            const uint64_t key = (static_cast<uint64_t>(request.sid) << 32) | request.function;
            RpcCallCounter &counter = m_callCounts[key];
            ++(served ? counter.served : counter.noServer);
        }
        if (!served)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    std::vector<std::string> IopRpcBridge::debugLines() const
    {
        std::vector<std::string> lines;
        char line[160];
        for (const auto &[sid, server] : m_servers)
        {
            std::snprintf(line, sizeof(line), "rpc_server sid=0x%x func=0x%x buf=0x%x", sid, server.function,
                          server.buffer);
            lines.emplace_back(line);
        }
        for (const auto &[key, count] : m_iopClientCalls)
        {
            std::snprintf(line, sizeof(line), "iop_client %s sid=0x%x calls=%llu",
                          (key >> 32) == 15u ? "sceSifBindRpc" : "sceSifCallRpc", static_cast<uint32_t>(key),
                          static_cast<unsigned long long>(count));
            lines.emplace_back(line);
        }
        std::snprintf(line, sizeof(line), "iop_client completions=%llu pending=%zu",
                      static_cast<unsigned long long>(m_iopClientCompletions), m_pendingClients.size());
        lines.emplace_back(line);
        for (const auto &[key, counter] : m_callCounts)
        {
            std::snprintf(line, sizeof(line), "rpc_call sid=0x%x fn=%u served=%llu no_server=%llu",
                          static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key),
                          static_cast<unsigned long long>(counter.served),
                          static_cast<unsigned long long>(counter.noServer));
            lines.emplace_back(line);
        }
        return lines;
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
