#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2: two cores of 24 ADPCM voices sharing 2 MB of sound RAM, clocked at 48 kHz.
    // Models what the IOP sound drivers (libsd, MultiStream) observe and rely on: register file, manual and DMA
    // transfers into sound RAM, ADPCM decoding with loop flags, ADSR envelopes, ENDX/NAX/ENVX state, the IRQA
    // address interrupt, and a stereo mix of the dry voice output. Reverb and the ADMA input path are not modelled.
    class IopSpu2
    {
    public:
        static constexpr uint32_t RegisterBase = 0x1F900000u;
        static constexpr uint32_t RegisterSize = 0x800u;
        static constexpr uint32_t RamHalfwords = 1u << 20; // 2 MB
        static constexpr uint32_t SampleRate = 48000u;
        static constexpr uint32_t CyclesPerSample = 768u; // 36.864 MHz IOP clock / 48 kHz

        IopSpu2();

        void reset();

        [[nodiscard]] static bool owns(uint32_t physicalAddress) noexcept
        {
            return physicalAddress >= RegisterBase && physicalAddress < RegisterBase + RegisterSize;
        }

        [[nodiscard]] uint16_t read16(uint32_t physicalAddress);
        void write16(uint32_t physicalAddress, uint16_t value);

        // IOP DMA channel 4 (core 0) / 7 (core 1). toSpu: IOP RAM -> sound RAM at the core's transfer address.
        void dma(int core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu);

        // Generates every 48 kHz sample up to the given IOP cycle.
        void advanceTo(uint64_t cycle);
        [[nodiscard]] uint64_t nextSampleCycle() const noexcept { return m_lastCycle + CyclesPerSample; }

        // True once after an IRQA hit (the caller raises IOP interrupt 9).
        [[nodiscard]] bool takeIrq() noexcept;

        // Interleaved stereo int16 frames generated since the last call.
        void takeOutput(std::vector<int16_t> &out);

        struct Stats
        {
            uint64_t keyOns = 0, dmaBytes = 0, manualWrites = 0, irqs = 0, frames = 0;
            int32_t peak = 0;
            uint32_t activeVoices = 0;
            uint32_t hotReadOffset = 0; // most-read register this period and its read count
            uint64_t hotReadCount = 0;
        };
        // Counters since the last call (activeVoices is a snapshot).
        [[nodiscard]] Stats takeStats() noexcept;

        [[nodiscard]] const std::vector<uint16_t> &ram() const noexcept { return m_ram; }
        [[nodiscard]] bool voiceActive(int core, int voice) const noexcept { return m_cores[core].voices[voice].active; }

    private:
        enum class EnvPhase : uint8_t
        {
            Attack,
            Decay,
            Sustain,
            Release,
            Off
        };

        struct Voice
        {
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0;
            uint32_t block = 0;       // address (halfwords) of the block being played
            bool customLoop = false;  // LSAX written by software: block loop-start flags don't move it
            bool active = false;
            int sampleIndex = 0;      // 0..27 within the decoded block
            uint32_t counter = 0;     // 12-bit fractional pitch accumulator
            uint8_t blockFlags = 0;
            std::array<int16_t, 28> decoded{};
            int16_t prev1 = 0, prev2 = 0;            // ADPCM filter history
            std::array<int16_t, 4> interp{};         // last four samples for interpolation
            EnvPhase phase = EnvPhase::Off;
            int32_t envelope = 0;
            uint32_t envWait = 0;
            int16_t outL = 0, outR = 0;              // VOLXL/VOLXR
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint32_t endx = 0;
            uint32_t tsa = 0;
            uint32_t irqa = 0;
            uint16_t attr = 0;
            uint16_t statx = 0; // bit 7: DMA ready (set by a DMA mode in ATTR and by DMA completion)
            uint32_t vmixl = 0, vmixr = 0;
        };

        uint16_t &reg(uint32_t offset) { return m_regs[(offset & (RegisterSize - 1u)) >> 1]; }
        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void decodeBlock(Voice &voice);
        void stepEnvelope(Voice &voice);
        void stepVoice(Core &core, int index, int32_t &left, int32_t &right);
        void checkIrq(uint32_t address, uint32_t halfwords);
        void tick();
        static int32_t volume(uint16_t raw);

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, RegisterSize / 2> m_regs{};
        std::array<Core, 2> m_cores{};
        uint16_t m_irqInfo = 0;
        bool m_irqPending = false;
        uint64_t m_lastCycle = 0;
        std::vector<int16_t> m_output;
        Stats m_stats;
        std::array<uint32_t, RegisterSize / 2> m_readCounts{};
    };
}
