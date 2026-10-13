#pragma once

#include "ps2_stubs.h"

#include <functional>
#include <vector>

namespace ps2_stubs
{
    bool dispatchSifCommand(uint8_t *rdram, PS2Runtime *runtime, uint32_t commandId, const void *packet, size_t packetSize) noexcept;
    // IOP -> EE RPC: look up an EE-registered server and run its function on the EE scheduler.
    uint32_t findEeRpcServer(uint32_t sid) noexcept;
    bool callEeRpcServer(uint8_t *rdram,
                         PS2Runtime *runtime,
                         uint32_t sid,
                         uint32_t rpcNumber,
                         const std::vector<uint8_t> &send,
                         uint32_t receiveSize,
                         std::function<void(const uint8_t *, size_t)> onReply);
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void resetSifState();
    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
