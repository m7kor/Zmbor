#pragma once

#include "ps2_syscalls.h"

#include <functional>
#include <vector>

namespace ps2_syscalls
{
    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // IOP-as-client RPC to EE servers (the EE side of IOP sceSifBindRpc/sceSifCallRpc). Only servers the EE
    // registered itself (sceSifRegisterRpc, with a recompiled server function) match; the zeroed descriptors the EE
    // bind path keeps for IOP servers do not.
    bool findEeRpcServerForIop(uint8_t *rdram, PS2Runtime *runtime, uint32_t sid, uint32_t &serverAddress,
                               uint32_t &bufferAddress);
    // Copies send into the server buffer and queues the server function (fno, buf, size) as an EE guest
    // invocation; onComplete receives receiveSize bytes from the pointer the function returned (empty for NULL).
    bool queueEeRpcServerCallForIop(uint8_t *rdram, PS2Runtime *runtime, uint32_t serverAddress, uint32_t function,
                                    std::vector<uint8_t> send, uint32_t receiveSize,
                                    std::function<void(const std::vector<uint8_t> &)> onComplete);
}
