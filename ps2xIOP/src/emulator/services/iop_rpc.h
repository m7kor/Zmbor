#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }
        // Bring-up diagnostics: registered servers and EE->IOP calls per (sid, function).
        [[nodiscard]] std::vector<std::string> debugLines() const;
        // Runs IOP end_function callbacks of IOP->EE calls (set by the emulator, which owns the executor).
        void setExecutor(IopGuestExecutor *executor) noexcept { m_executor = executor; }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        struct RpcCallCounter
        {
            uint64_t served = 0;
            uint64_t noServer = 0;
        };
        std::map<uint64_t, RpcCallCounter> m_callCounts;
        std::map<uint64_t, uint64_t> m_iopClientCalls; // (ordinal << 32 | sid) -> calls
        // IOP-as-client calls to EE servers in flight (client descriptor addresses) and the sid each client bound.
        std::unordered_set<uint32_t> m_pendingClients;
        std::unordered_map<uint32_t, uint32_t> m_clientSids;
        uint64_t m_iopClientCompletions = 0;
        IopGuestExecutor *m_executor = nullptr;
        static constexpr uint32_t kClientDataSize = 0x28u;
        static constexpr uint32_t kRpcModeNowait = 0x1u;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
