#include "emulator/core/iop_spu2.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using ps2x::iop::detail::IopSpu2;

namespace
{
    int g_failures = 0;

    void check(bool condition, const char *what)
    {
        std::printf("  [%s] %s\n", condition ? "Passed" : "FAILED", what);
        if (!condition)
            ++g_failures;
    }

    constexpr uint32_t kBase = IopSpu2::RegisterBase;

    void write(IopSpu2 &spu, uint32_t offset, uint16_t value) { spu.write16(kBase + offset, value); }
    uint16_t read(IopSpu2 &spu, uint32_t offset) { return spu.read16(kBase + offset); }

    // Writes halfwords into sound RAM through the core 0 manual-transfer port.
    void upload(IopSpu2 &spu, uint32_t address, const std::vector<uint16_t> &data)
    {
        write(spu, 0x1A8, static_cast<uint16_t>(address >> 16));
        write(spu, 0x1AA, static_cast<uint16_t>(address));
        for (uint16_t value : data)
            write(spu, 0x1AC, value);
    }

    // One ADPCM block: shift 0, filter 0, every nibble = 1 (sample 0x1000), with the given flags.
    std::vector<uint16_t> block(uint8_t flags)
    {
        std::vector<uint16_t> out{static_cast<uint16_t>(flags << 8)};
        for (int i = 0; i < 7; ++i)
            out.push_back(0x1111u);
        return out;
    }

    void runSamples(IopSpu2 &spu, uint64_t &cycle, int samples)
    {
        cycle += static_cast<uint64_t>(samples) * IopSpu2::CyclesPerSample;
        spu.advanceTo(cycle);
    }
}

int main()
{
    std::printf("[Suite]: IopSpu2\n");

    {
        IopSpu2 spu;
        upload(spu, 0x1000, {0x1234, 0x5678});
        check(spu.ram()[0x1000] == 0x1234 && spu.ram()[0x1001] == 0x5678, "data port writes sound RAM at TSA and advances it");
        check(read(spu, 0x1AA) == 0x1002, "TSA reads back advanced");
    }

    {
        IopSpu2 spu;
        check(read(spu, 0x344) == 0u, "STATX idle after reset (libsd waits for STATX & 0x7FF == 0)");
        std::vector<uint8_t> iopRam(0x10000, 0);
        for (int i = 0; i < 64; ++i)
            iopRam[0x100 + i] = static_cast<uint8_t>(i);
        write(spu, 0x1A8, 0);
        write(spu, 0x1AA, 0x2000);
        write(spu, 0x19E, 0x2010); // IRQA inside the transfer
        write(spu, 0x19A, 0x40);   // IRQ enable
        spu.dma(0, iopRam.data(), static_cast<uint32_t>(iopRam.size()), 0x100, 64, true);
        check(spu.ram()[0x2000] == 0x0100 && spu.ram()[0x201F] == 0x3F3E, "DMA copies IOP RAM to sound RAM at TSA");
        check((read(spu, 0x344) & 0x80u) != 0u, "STATX reports DMA ready");
        check(spu.takeIrq() && (read(spu, 0x7C2) & 0x4u) != 0u, "DMA over IRQA raises the core 0 IRQ");
        write(spu, 0x19A, 0x00);
        check((read(spu, 0x7C2) & 0x4u) == 0u, "clearing IRQ enable clears IRQINFO");
    }

    {
        // A one-shot sample: two blocks, loop-end on the second. Voice 0 of core 0, pitch 1.0, instant attack.
        IopSpu2 spu;
        std::vector<uint16_t> data = block(0x4);
        const std::vector<uint16_t> last = block(0x1);
        data.insert(data.end(), last.begin(), last.end());
        upload(spu, 0x3000, data);
        write(spu, 0x188, 0xFFFF);  // VMIXL
        write(spu, 0x190, 0xFFFF);  // VMIXR
        write(spu, 0x760, 0x3FFF);  // core 0 MVOL
        write(spu, 0x762, 0x3FFF);
        write(spu, 0x788, 0x3FFF);  // core 1 MVOL
        write(spu, 0x78A, 0x3FFF);
        write(spu, 0x000, 0x3FFF);  // VOLL
        write(spu, 0x002, 0x3FFF);  // VOLR
        write(spu, 0x004, 0x1000);  // pitch 1.0
        write(spu, 0x006, 0x000F);  // ADSR1: fastest linear attack, sustain level max
        write(spu, 0x008, 0x0000);  // ADSR2
        write(spu, 0x1C0, 0x0000);  // SSA
        write(spu, 0x1C2, 0x3000);
        write(spu, 0x1A0, 0x0001);  // KON voice 0
        uint64_t cycle = 0;
        runSamples(spu, cycle, 20);
        check(spu.voiceActive(0, 0), "voice plays after KON");
        check(read(spu, 0x00A) > 0x7000, "attack raises ENVX quickly");
        std::vector<int16_t> out;
        spu.takeOutput(out);
        int16_t peak = 0;
        for (int16_t s : out)
            peak = std::max<int16_t>(peak, s);
        check(peak > 0x200, "voice output reaches the mix");
        const uint16_t nax = read(spu, 0x1CA);
        check(nax > 0x3000 && nax < 0x3008, "NAX points inside the first block");
        check((read(spu, 0x340) & 1u) == 0u, "ENDX clear while playing");
        runSamples(spu, cycle, 60);
        check((read(spu, 0x340) & 1u) != 0u, "ENDX set after the loop-end block");
        check(!spu.voiceActive(0, 0), "one-shot voice stops at its end");
    }

    {
        // Looping sample: loop-start + loop-end + repeat in one block keeps the voice alive; IRQA on it fires.
        IopSpu2 spu;
        upload(spu, 0x4000, block(0x7));
        write(spu, 0x004, 0x1000);
        write(spu, 0x006, 0x000F);
        write(spu, 0x1C2, 0x4000);
        write(spu, 0x19E, 0x4000);
        write(spu, 0x19A, 0x40);
        write(spu, 0x1A0, 0x0001);
        uint64_t cycle = 0;
        runSamples(spu, cycle, 100);
        check(spu.voiceActive(0, 0), "looping voice keeps playing");
        check((read(spu, 0x340) & 1u) != 0u, "looping voice still flags ENDX");
        check(spu.takeIrq(), "voice reading the IRQA block raises the IRQ");
        write(spu, 0x1A4, 0x0001); // KOFF
        runSamples(spu, cycle, 48000);
        check(!spu.voiceActive(0, 0), "KOFF releases the voice to silence");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
