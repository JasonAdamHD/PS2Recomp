#include <string>
#include <chrono>
#include <deque>
#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdlib>
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
                static uint32_t iopDmaLogCount = 0u;
                static const bool logDma = std::getenv("PS2X_IOP_LOG") != nullptr;
                if (logDma && (++iopDmaLogCount <= 300u || (iopDmaLogCount % 500u) == 0u))
                {
                    char line[200];
                    std::snprintf(line, sizeof(line), "[IOP:sifdma#%u] src=0x%x dst=0x%x size=0x%x attr=0x%x owned=%d data=%x %x %x %x",
                                  iopDmaLogCount, source, destination, size, descriptorWords[i * 4u + 3u],
                                  m_memory.ownsRamRange(source, size) ? 1 : 0, m_memory.read32(source),
                                  m_memory.read32(source + 4u), m_memory.read32(source + 8u), m_memory.read32(source + 12u));
                    m_host.log(LogLevel::Info, line);
                }
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
        case 14: // InitRpc
            setV0(0);
            return true;
        case 15: // sceSifBindRpc(client, sid, mode) against an EE-side server
        {
            constexpr uint32_t kClientServerOffset = 0x24u;
            const uint32_t client = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            const bool bound = client != 0u && m_host.hasEeRpcServer(sid);
            if (client != 0u)
            {
                // IOP modules only test the server pointer for non-zero; the client address is a stable token.
                (void)m_memory.write32(client + kClientServerOffset, bound ? client : 0u);
                if (bound)
                    m_eeRpcClients[client] = sid;
            }
            static uint32_t bindLogCount = 0u;
            if (++bindLogCount <= 16u)
            {
                char line[96];
                std::snprintf(line, sizeof(line), "[IOP:eerpc] bind client=0x%x sid=0x%x bound=%d", client, sid, bound ? 1 : 0);
                m_host.log(LogLevel::Info, line);
            }
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc(client, rpcNumber, mode, send, ssize, recv, rsize, endFunc, endParam)
        {
            const uint32_t client = cpu.gpr[4];
            const uint32_t rpcNumber = cpu.gpr[5];
            const uint32_t send = cpu.gpr[7];
            const uint32_t sp = cpu.gpr[29];
            const uint32_t sendSize = m_memory.read32(sp + 0x10u);
            const uint32_t receive = m_memory.read32(sp + 0x14u);
            const uint32_t receiveSize = m_memory.read32(sp + 0x18u);
            const auto clientIt = m_eeRpcClients.find(client);
            if (clientIt == m_eeRpcClients.end() || sendSize > 0x10000u || receiveSize > 0x10000u)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            std::vector<uint8_t> payload(sendSize);
            if (sendSize != 0u && !m_memory.readRam(send, payload.data(), payload.size()))
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            // The EE server runs on the EE scheduler; its reply lands in the IOP receive buffer when it returns.
            // A blocking call from a scheduled thread sleeps until then, so code reading the reply right
            // after sceSifCallRpc sees it. Nested host-driven calls cannot block and continue immediately.
            constexpr uint32_t kRpcModeNoWait = 1u;
            const uint32_t mode = cpu.gpr[6];
            const int waitingThread = (mode & kRpcModeNoWait) == 0u ? m_kernel.threadIdForCpu(cpu) : 0;
            const bool queued = m_host.callEeRpc(clientIt->second, rpcNumber, payload, receiveSize,
                                                 [this, receive, waitingThread](const uint8_t *data, size_t size)
                                                 {
                                                     if (receive != 0u && size != 0u)
                                                         (void)m_memory.writeRam(receive, data, size);
                                                     if (waitingThread != 0)
                                                         m_kernel.wakeSleepingThread(waitingThread);
                                                 });
            if (queued && waitingThread != 0)
                m_kernel.sleepCurrent(cpu);
            static uint32_t callLogCount = 0u;
            if (++callLogCount <= 40u)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "[IOP:eerpc] call sid=0x%x fno=%u send=0x%x/0x%x recv=0x%x/0x%x queued=%d thread=%d",
                              clientIt->second, rpcNumber, send, sendSize, receive, receiveSize, queued ? 1 : 0, waitingThread);
                m_host.log(LogLevel::Info, line);
            }
            setV0(queued ? 0u : 0xFFFFFFFFu);
            return true;
        }
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
        case 18:
            setV0(0);
            return true;
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

    namespace
    {
        // PS2X_ATCMD_RING=1: keep the last 64 Atlas IO commands/replies in memory and print them every 30 s.
        // Unlike streaming them to the log, this barely changes timing (the load stall it is for hides when logged).
        bool atRingEnabled()
        {
            static const bool enabled = std::getenv("PS2X_ATCMD_RING") != nullptr;
            return enabled;
        }

        void atRingPush(std::string text)
        {
            static std::deque<std::string> ring;
            static auto lastDump = std::chrono::steady_clock::now();
            static uint64_t sequence = 0u;
            ring.push_back(std::to_string(++sequence) + " " + std::move(text));
            if (ring.size() > 64u)
                ring.pop_front();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastDump >= std::chrono::seconds(30))
            {
                lastDump = now;
                std::string dump = "[IOP:atring]";
                for (const std::string &line : ring)
                    dump += "\n  " + line;
                std::fprintf(stderr, "%s\n", dump.c_str());
            }
        }
    }

    void IopRpcBridge::logCommandPacket(uint32_t sid, const std::vector<uint8_t> &payload)
    {
        // Decodes the Atlas IO packet (0x48, count, {id, argc, args...}) so new EE requests show up in the log.
        static const bool enabled = std::getenv("PS2X_IOP_LOG") != nullptr || std::getenv("PS2X_ATCMD_LOG") != nullptr || atRingEnabled();
        if (!enabled || sid != 0x12345u || payload.size() < 4u)
            return;
        const auto half = [&](size_t index) -> uint16_t
        {
            return index * 2u + 1u < payload.size() ? static_cast<uint16_t>(payload[index * 2u] | (payload[index * 2u + 1u] << 8u)) : 0u;
        };
        if (half(0) != 0x48u)
            return;
        std::string text;
        size_t cursor = 2u;
        const uint16_t count = half(1);
        for (uint16_t i = 0; i < count && cursor * 2u + 3u < payload.size(); ++i)
        {
            const uint16_t id = half(cursor);
            const uint16_t argc = half(cursor + 1u);
            cursor += 2u;
            char entry[32];
            std::snprintf(entry, sizeof(entry), " %x(", id);
            text += entry;
            for (uint16_t a = 0; a < argc && a < 12u; ++a)
            {
                std::snprintf(entry, sizeof(entry), a ? ",%x" : "%x", half(cursor + a));
                text += entry;
            }
            text += ")";
            cursor += argc;
        }
        if (text == m_lastCommandText)
            return;
        m_lastCommandText = text;
        if (atRingEnabled())
        {
            atRingPush("cmd" + text);
            return;
        }
        static uint32_t logged = 0u;
        if (++logged <= 3000u)
            m_host.log(std::getenv("PS2X_ATCMD_LOG") ? LogLevel::Warning : LogLevel::Info, "[IOP:atcmd]" + text);
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                {
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
                    logCommandPacket(request.sid, payload);
                }
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

        static const bool logReplies = std::getenv("PS2X_IOP_LOG") != nullptr || std::getenv("PS2X_ATCMD_LOG") != nullptr || atRingEnabled();
        if (logReplies && request.sid == 0x12345u && returnPointer != 0u)
        {
            // Reply layout: word0 = record count | flags << 8, then 9-word slot records, then a status tail.
            const uint32_t header = m_memory.read32(returnPointer);
            const uint32_t records = header & 0xFFu;
            std::string text;
            char entry[96];
            for (uint32_t r = 0; r < records && r < 8u; ++r)
            {
                const uint32_t base = returnPointer + 4u + r * 36u;
                std::snprintf(entry, sizeof(entry), " rec{w0=%x type=%x done=%x}", m_memory.read32(base),
                              m_memory.read32(base + 12u) & 0xFFu, m_memory.read32(base + 20u));
                text += entry;
            }
            const uint32_t tail = returnPointer + 4u + records * 36u;
            std::snprintf(entry, sizeof(entry), " tail{block=%x busy=%x}", m_memory.read32(tail + 4u), m_memory.read32(tail + 8u));
            text += entry;
            if (text != m_lastReplyText)
            {
                m_lastReplyText = text;
                if (atRingEnabled())
                {
                    atRingPush("reply n=" + std::to_string(records) + text);
                }
                static uint32_t logged = 0u;
                if (atRingEnabled())
                    logged = 3000u;
                if (++logged <= 3000u)
                    m_host.log(std::getenv("PS2X_ATCMD_LOG") ? LogLevel::Warning : LogLevel::Info, "[IOP:atreply] n=" + std::to_string(records) + text);
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

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
