#include "iop_spu2.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kAddrMask = IopSpu2::RamHalfwords - 1u;

        // Per-core register offsets (core 1 is +0x400).
        constexpr uint32_t kVmixl = 0x188u, kVmixr = 0x190u;
        constexpr uint32_t kAttr = 0x19Au, kIrqaH = 0x19Cu, kIrqaL = 0x19Eu;
        constexpr uint32_t kKon = 0x1A0u, kKoff = 0x1A4u, kTsaH = 0x1A8u, kTsaL = 0x1AAu, kData = 0x1ACu;
        constexpr uint32_t kVoiceAddrBase = 0x1C0u, kVoiceAddrEnd = 0x1C0u + 24u * 0xCu;
        constexpr uint32_t kEndx = 0x340u, kStatx = 0x344u;
        constexpr uint32_t kMvol = 0x760u, kMvolStride = 0x28u;
        constexpr uint32_t kIrqInfo = 0x7C2u;

        constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};

        int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        uint32_t setHigh(uint32_t address, uint16_t value) { return ((static_cast<uint32_t>(value) & 0xFu) << 16) | (address & 0xFFFFu); }
        uint32_t setLow(uint32_t address, uint16_t value) { return (address & 0xF0000u) | value; }
    }

    IopSpu2::IopSpu2()
        : m_ram(RamHalfwords)
    {
        reset();
    }

    void IopSpu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0);
        m_cores = {};
        m_irqInfo = 0;
        m_irqPending = false;
        m_lastCycle = 0;
        m_output.clear();
    }

    int32_t IopSpu2::volume(uint16_t raw)
    {
        // Bit 15 selects a volume sweep; the sweep itself is not modelled, so it plays at full volume.
        if (raw & 0x8000u)
            return 0x7FFF;
        return static_cast<int16_t>(static_cast<uint16_t>(raw << 1));
    }

    uint16_t IopSpu2::read16(uint32_t physicalAddress)
    {
        const uint32_t offset = physicalAddress - RegisterBase;
        ++m_readCounts[offset >> 1];
        if (offset == kIrqInfo)
            return m_irqInfo;
        if (offset >= kMvol)
            return reg(offset);

        const int c = offset >= 0x400u ? 1 : 0;
        Core &core = m_cores[c];
        const uint32_t r = offset & 0x3FFu;
        if (r < 0x180u)
        {
            const Voice &v = core.voices[r >> 4];
            switch (r & 0xFu)
            {
            case 0xA:
                return static_cast<uint16_t>(v.envelope);
            case 0xC:
                return static_cast<uint16_t>(v.outL);
            case 0xE:
                return static_cast<uint16_t>(v.outR);
            default:
                return reg(offset);
            }
        }
        if (r >= kVoiceAddrBase && r < kVoiceAddrEnd)
        {
            const Voice &v = core.voices[(r - kVoiceAddrBase) / 0xCu];
            const uint32_t nax = (v.block + 1u + static_cast<uint32_t>(v.sampleIndex) / 4u) & kAddrMask;
            switch ((r - kVoiceAddrBase) % 0xCu)
            {
            case 0x0: return static_cast<uint16_t>(v.ssa >> 16);
            case 0x2: return static_cast<uint16_t>(v.ssa);
            case 0x4: return static_cast<uint16_t>(v.lsa >> 16);
            case 0x6: return static_cast<uint16_t>(v.lsa);
            case 0x8: return static_cast<uint16_t>(nax >> 16);
            default: return static_cast<uint16_t>(nax);
            }
        }
        switch (r)
        {
        case kAttr: return core.attr;
        case kIrqaH: return static_cast<uint16_t>(core.irqa >> 16);
        case kIrqaL: return static_cast<uint16_t>(core.irqa);
        case kTsaH: return static_cast<uint16_t>(core.tsa >> 16);
        case kTsaL: return static_cast<uint16_t>(core.tsa);
        case kData:
        {
            const uint16_t value = m_ram[core.tsa];
            core.tsa = (core.tsa + 1u) & kAddrMask;
            return value;
        }
        case kEndx: return static_cast<uint16_t>(core.endx);
        case kEndx + 2u: return static_cast<uint16_t>(core.endx >> 16);
        case kStatx: return core.statx;
        default: return reg(offset);
        }
    }

    void IopSpu2::write16(uint32_t physicalAddress, uint16_t value)
    {
        const uint32_t offset = physicalAddress - RegisterBase;
        reg(offset) = value;
        if (offset >= kMvol)
            return;

        const int c = offset >= 0x400u ? 1 : 0;
        Core &core = m_cores[c];
        const uint32_t r = offset & 0x3FFu;
        if (r < 0x180u)
        {
            Voice &v = core.voices[r >> 4];
            switch (r & 0xFu)
            {
            case 0x0: v.volL = value; break;
            case 0x2: v.volR = value; break;
            case 0x4: v.pitch = value; break;
            case 0x6: v.adsr1 = value; break;
            case 0x8: v.adsr2 = value; break;
            case 0xA: v.envelope = value & 0x7FFF; break;
            default: break;
            }
            return;
        }
        if (r >= kVoiceAddrBase && r < kVoiceAddrEnd)
        {
            Voice &v = core.voices[(r - kVoiceAddrBase) / 0xCu];
            switch ((r - kVoiceAddrBase) % 0xCu)
            {
            case 0x0: v.ssa = setHigh(v.ssa, value); break;
            case 0x2: v.ssa = setLow(v.ssa, value); break;
            case 0x4: v.lsa = setHigh(v.lsa, value); v.customLoop = true; break;
            case 0x6: v.lsa = setLow(v.lsa, value); v.customLoop = true; break;
            default: break; // NAX is read-only in practice
            }
            return;
        }
        switch (r)
        {
        case kVmixl: core.vmixl = (core.vmixl & 0xFF0000u) | value; break;
        case kVmixl + 2u: core.vmixl = (core.vmixl & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case kVmixr: core.vmixr = (core.vmixr & 0xFF0000u) | value; break;
        case kVmixr + 2u: core.vmixr = (core.vmixr & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case kAttr:
            // Entering a DMA/transfer mode (bits 4-5) reports ready; leaving it clears the ready bit.
            if ((value & 0x30u) == 0u)
                core.statx &= static_cast<uint16_t>(~0x80u);
            else if ((core.attr & 0x30u) == 0u)
                core.statx |= 0x80u;
            core.attr = value;
            if ((value & 0x40u) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << c));
            break;
        case kIrqaH: core.irqa = setHigh(core.irqa, value); break;
        case kIrqaL: core.irqa = setLow(core.irqa, value); break;
        case kKon: keyOn(c, value); break;
        case kKon + 2u: keyOn(c, static_cast<uint32_t>(value & 0xFFu) << 16); break;
        case kKoff: keyOff(c, value); break;
        case kKoff + 2u: keyOff(c, static_cast<uint32_t>(value & 0xFFu) << 16); break;
        case kTsaH: core.tsa = setHigh(core.tsa, value); break;
        case kTsaL: core.tsa = setLow(core.tsa, value); break;
        case kData:
            m_ram[core.tsa] = value;
            ++m_stats.manualWrites;
            checkIrq(core.tsa, 1u);
            core.tsa = (core.tsa + 1u) & kAddrMask;
            break;
        case kEndx: core.endx &= 0xFF0000u; break;
        case kEndx + 2u: core.endx &= 0xFFFFu; break;
        default: break;
        }
    }

    void IopSpu2::dma(int c, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu)
    {
        Core &core = m_cores[c & 1];
        madr &= iopRamSize - 1u;
        bytes = std::min(bytes, iopRamSize - madr);
        const uint32_t halfwords = bytes / 2u;
        for (uint32_t i = 0; i < halfwords; ++i)
        {
            uint16_t *spu = &m_ram[(core.tsa + i) & kAddrMask];
            if (toSpu)
                std::memcpy(spu, iopRam + madr + i * 2u, sizeof(uint16_t));
            else
                std::memcpy(iopRam + madr + i * 2u, spu, sizeof(uint16_t));
        }
        m_stats.dmaBytes += bytes;
        checkIrq(core.tsa, halfwords);
        core.tsa = (core.tsa + halfwords) & kAddrMask;
        core.statx |= 0x80u;
    }

    void IopSpu2::checkIrq(uint32_t address, uint32_t halfwords)
    {
        for (int c = 0; c < 2; ++c)
        {
            const Core &core = m_cores[c];
            if ((core.attr & 0x40u) == 0u)
                continue;
            if (((core.irqa - address) & kAddrMask) < halfwords)
            {
                if ((m_irqInfo & (4u << c)) == 0u)
                    ++m_stats.irqs;
                m_irqInfo |= static_cast<uint16_t>(4u << c);
                m_irqPending = true;
            }
        }
    }

    bool IopSpu2::takeIrq() noexcept
    {
        const bool pending = m_irqPending;
        m_irqPending = false;
        return pending;
    }

    void IopSpu2::keyOn(int c, uint32_t mask)
    {
        Core &core = m_cores[c];
        for (int i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = core.voices[i];
            ++m_stats.keyOns;
            v.active = true;
            v.customLoop = false;
            v.block = v.ssa & ~7u;
            v.sampleIndex = 0;
            v.counter = 0;
            v.prev1 = v.prev2 = 0;
            v.interp = {};
            v.phase = EnvPhase::Attack;
            v.envelope = 0;
            v.envWait = 0;
            core.endx &= ~(1u << i);
            decodeBlock(v);
        }
    }

    void IopSpu2::keyOff(int c, uint32_t mask)
    {
        Core &core = m_cores[c];
        for (int i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = core.voices[i];
            if (v.active && v.phase != EnvPhase::Off)
            {
                v.phase = EnvPhase::Release;
                v.envWait = 0;
            }
        }
    }

    void IopSpu2::decodeBlock(Voice &v)
    {
        checkIrq(v.block, 8u);
        const uint16_t header = m_ram[v.block & kAddrMask];
        int shift = header & 0xF;
        if (shift > 12)
            shift = 9;
        const int filter = std::min((header >> 4) & 0x7, 4);
        v.blockFlags = static_cast<uint8_t>(header >> 8);
        if ((v.blockFlags & 0x4u) && !v.customLoop)
            v.lsa = v.block;

        for (int i = 0; i < 28; ++i)
        {
            const uint16_t word = m_ram[(v.block + 1u + static_cast<uint32_t>(i / 4)) & kAddrMask];
            const int nibble = (word >> ((i & 3) * 4)) & 0xF;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            sample += (v.prev1 * kFilterPos[filter] + v.prev2 * kFilterNeg[filter] + 32) >> 6;
            const int16_t clamped = clamp16(sample);
            v.decoded[i] = clamped;
            v.prev2 = v.prev1;
            v.prev1 = clamped;
        }
    }

    void IopSpu2::stepEnvelope(Voice &v)
    {
        bool increase = false;
        bool exponential = false;
        int shift = 0;
        int step = 0;
        switch (v.phase)
        {
        case EnvPhase::Attack:
            increase = true;
            exponential = (v.adsr1 & 0x8000u) != 0u;
            shift = (v.adsr1 >> 10) & 0x1F;
            step = 7 - ((v.adsr1 >> 8) & 3);
            break;
        case EnvPhase::Decay:
            exponential = true;
            shift = ((v.adsr1 >> 4) & 0xF) << 2;
            step = -8;
            break;
        case EnvPhase::Sustain:
            exponential = (v.adsr2 & 0x8000u) != 0u;
            increase = (v.adsr2 & 0x4000u) == 0u;
            shift = (v.adsr2 >> 8) & 0x1F;
            step = increase ? 7 - ((v.adsr2 >> 6) & 3) : -8 + ((v.adsr2 >> 6) & 3);
            break;
        case EnvPhase::Release:
            exponential = (v.adsr2 & 0x20u) != 0u;
            shift = (v.adsr2 & 0x1F) << 2;
            step = -8;
            break;
        case EnvPhase::Off:
            return;
        }

        if (v.envWait > 0u)
        {
            --v.envWait;
        }
        else
        {
            uint32_t cycles = 1u << std::max(0, shift - 11);
            int32_t delta = step * (1 << std::max(0, 11 - shift));
            if (exponential && increase && v.envelope > 0x6000)
                cycles *= 4u;
            if (exponential && !increase)
                delta = (delta * v.envelope) >> 15;
            v.envWait = cycles - 1u;
            v.envelope = std::clamp(v.envelope + delta, 0, 0x7FFF);
        }

        switch (v.phase)
        {
        case EnvPhase::Attack:
            if (v.envelope >= 0x7FFF)
            {
                v.phase = EnvPhase::Decay;
                v.envWait = 0;
            }
            break;
        case EnvPhase::Decay:
            if (v.envelope <= std::min<int32_t>(((v.adsr1 & 0xF) + 1) * 0x800, 0x7FFF))
            {
                v.phase = EnvPhase::Sustain;
                v.envWait = 0;
            }
            break;
        case EnvPhase::Release:
            if (v.envelope <= 0)
            {
                v.phase = EnvPhase::Off;
                v.active = false;
            }
            break;
        default:
            break;
        }
    }

    void IopSpu2::stepVoice(Core &core, int index, int32_t &left, int32_t &right)
    {
        Voice &v = core.voices[index];
        // 4-point Catmull-Rom between interp[1] and interp[2] (the hardware uses a Gaussian table).
        const int32_t t = static_cast<int32_t>(v.counter & 0xFFFu);
        const int32_t p0 = v.interp[0], p1 = v.interp[1], p2 = v.interp[2], p3 = v.interp[3];
        const int64_t a = -p0 + 3 * p1 - 3 * p2 + p3;
        const int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
        const int64_t c = -p0 + p2;
        const int64_t tt = t;
        const int64_t value = (((((a * tt) >> 12) + b) * tt >> 12) + c) * tt / 8192 + p1;
        const int32_t sample = clamp16(static_cast<int32_t>(value));

        const int32_t enveloped = (sample * v.envelope) >> 15;
        v.outL = clamp16((enveloped * volume(v.volL)) >> 15);
        v.outR = clamp16((enveloped * volume(v.volR)) >> 15);
        left = v.outL;
        right = v.outR;

        stepEnvelope(v);
        if (!v.active)
            return;

        v.counter += std::min<uint32_t>(v.pitch, 0x3FFFu);
        while (v.counter >= 0x1000u && v.active)
        {
            v.counter -= 0x1000u;
            v.interp = {v.interp[1], v.interp[2], v.interp[3], v.decoded[v.sampleIndex]};
            if (++v.sampleIndex < 28)
                continue;
            v.sampleIndex = 0;
            const uint8_t flags = v.blockFlags;
            if (flags & 0x1u)
            {
                // End of the sample: flag ENDX; loop to LSA if the repeat bit is set, otherwise the voice stops.
                core.endx |= 1u << index;
                if (flags & 0x2u)
                {
                    v.block = v.lsa & ~7u;
                }
                else
                {
                    v.block = v.lsa & ~7u;
                    v.envelope = 0;
                    v.phase = EnvPhase::Off;
                    v.active = false;
                    break;
                }
            }
            else
            {
                v.block = (v.block + 8u) & kAddrMask;
            }
            decodeBlock(v);
        }
    }

    void IopSpu2::tick()
    {
        int32_t mix[2][2] = {};
        for (int c = 0; c < 2; ++c)
        {
            Core &core = m_cores[c];
            for (int i = 0; i < 24; ++i)
            {
                Voice &v = core.voices[i];
                if (!v.active)
                {
                    v.outL = v.outR = 0;
                    continue;
                }
                int32_t l = 0, r = 0;
                stepVoice(core, i, l, r);
                if (core.vmixl & (1u << i))
                    mix[c][0] += l;
                if (core.vmixr & (1u << i))
                    mix[c][1] += r;
            }
        }

        // Core 0's output feeds core 1, whose output is the final mix; each core applies its master volume.
        int32_t out[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            const int32_t core0 = (clamp16(mix[0][ch]) * volume(m_regs[(kMvol + ch * 2u) >> 1])) >> 15;
            const int32_t core1 = clamp16(mix[1][ch] + core0);
            out[ch] = (core1 * volume(m_regs[(kMvol + kMvolStride + ch * 2u) >> 1])) >> 15;
        }
        ++m_stats.frames;
        m_stats.peak = std::max({m_stats.peak, std::abs(out[0]), std::abs(out[1])});
        if (m_output.size() < SampleRate * 2u)
        {
            m_output.push_back(clamp16(out[0]));
            m_output.push_back(clamp16(out[1]));
        }
    }

    void IopSpu2::advanceTo(uint64_t cycle)
    {
        while (m_lastCycle + CyclesPerSample <= cycle)
        {
            m_lastCycle += CyclesPerSample;
            tick();
        }
    }

    IopSpu2::Stats IopSpu2::takeStats() noexcept
    {
        Stats stats = m_stats;
        m_stats = {};
        for (uint32_t i = 0; i < m_readCounts.size(); ++i)
        {
            if (m_readCounts[i] > stats.hotReadCount)
            {
                stats.hotReadCount = m_readCounts[i];
                stats.hotReadOffset = i * 2u;
            }
        }
        m_readCounts.fill(0);
        for (const Core &core : m_cores)
            for (const Voice &v : core.voices)
                stats.activeVoices += v.active ? 1u : 0u;
        return stats;
    }

    void IopSpu2::takeOutput(std::vector<int16_t> &out)
    {
        out.insert(out.end(), m_output.begin(), m_output.end());
        m_output.clear();
    }
}
