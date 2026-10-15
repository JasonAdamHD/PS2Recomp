#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/types.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr float kFltMax = 3.402823466e+38f;

    // COP2 macro-mode encodings (EE Core Instruction Set Manual, VU0 macro instructions).
    constexpr uint32_t cop2Special1(uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct)
    {
        return (0x12u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
    }

    constexpr uint32_t cop2Special2(uint32_t dest, uint32_t ft, uint32_t fs, uint32_t func)
    {
        return (0x12u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((func >> 2) << 6) | 0x3Cu | (func & 3u);
    }

    constexpr uint32_t cfc2(uint32_t rt, uint32_t reg)
    {
        return (0x12u << 26) | (0x02u << 21) | (rt << 16) | (reg << 11);
    }

    constexpr uint32_t kDestX = 0x8u;
    constexpr uint32_t kDestXYZ = 0xEu;
    constexpr uint32_t kDestXYZW = 0xFu;

    std::vector<ps2recomp::Instruction> decodeAll(uint32_t start, const std::vector<uint32_t> &raws)
    {
        ps2recomp::R5900Decoder decoder;
        std::vector<ps2recomp::Instruction> out;
        for (size_t i = 0; i < raws.size(); ++i)
            out.push_back(decoder.decodeInstruction(start + static_cast<uint32_t>(i * 4), raws[i]));
        return out;
    }

    std::string generate(const std::vector<uint32_t> &raws)
    {
        ps2recomp::Function func;
        func.name = "vu0_flags_probe";
        func.start = 0x10000;
        func.end = func.start + static_cast<uint32_t>(raws.size() * 4);
        func.isRecompiled = true;
        ps2recomp::CodeGenerator gen({}, {});
        return gen.generateFunction(func, decodeAll(func.start, raws), false);
    }

    size_t countOf(const std::string &haystack, const std::string &needle)
    {
        size_t n = 0;
        for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1))
            ++n;
        return n;
    }

    // The game's normalise helpers: VMUL.xyz vf2,vf1,vf1; VADDy.x vf2,vf2,vf2; VADDz.x vf2,vf2,vf2; CFC2 v0,$17.
    std::vector<uint32_t> normaliseHelper()
    {
        return {
            cop2Special1(kDestXYZ, 1, 1, 2, ps2recomp::VU0_S1_VMUL),
            cop2Special1(kDestX, 2, 2, 2, ps2recomp::VU0_S1_VADDy),
            cop2Special1(kDestX, 2, 2, 2, ps2recomp::VU0_S1_VADDz),
            cfc2(2, ps2recomp::VU0_CR_MAC),
        };
    }

    __m128 vec(float x, float y, float z, float w) { return _mm_setr_ps(x, y, z, w); }
}

void register_vu0_flag_tests()
{
    MiniTest::Case("VU0MacroFlags", [](TestCase &tc)
    {
        tc.Run("FMAC ops update MAC only in functions that read CFC2 $17/$16", [](TestCase &t)
        {
            const std::vector<uint32_t> normalise = normaliseHelper();
            const std::string withRead = generate(normalise);
            t.Equals(countOf(withRead, "PS2_VU0_FMAC_FLAGS(ctx, res, 0xEu)"), size_t(1), "VMUL.xyz should update flags with dest xyz");
            t.Equals(countOf(withRead, "PS2_VU0_FMAC_FLAGS(ctx, res, 0x8u)"), size_t(2), "VADDy.x / VADDz.x should update flags with dest x");

            std::vector<uint32_t> noRead(normalise.begin(), normalise.end() - 1);
            t.Equals(countOf(generate(noRead), "PS2_VU0_FMAC_FLAGS"), size_t(0), "functions without CFC2 $16/$17 pay nothing");

            std::vector<uint32_t> statusRead = noRead;
            statusRead.push_back(cfc2(2, ps2recomp::VU0_CR_STATUS));
            t.Equals(countOf(generate(statusRead), "PS2_VU0_FMAC_FLAGS"), size_t(3), "CFC2 $16 also enables tracking");
        });

        tc.Run("all FMAC families are tracked, non-FMAC ops are not", [](TestCase &t)
        {
            const std::vector<uint32_t> ops = {
                cop2Special2(kDestXYZW, 1, 2, ps2recomp::VU0_S2_VMULAx),
                cop2Special2(kDestXYZW, 1, 2, ps2recomp::VU0_S2_VMADDAy),
                cop2Special2(kDestXYZW, 1, 2, ps2recomp::VU0_S2_VMSUBA),
                cop2Special2(kDestXYZW, 1, 2, ps2recomp::VU0_S2_VADDAi),
                cop2Special2(kDestXYZW, 1, 2, ps2recomp::VU0_S2_VSUBAq),
                cop2Special2(kDestXYZ, 1, 2, ps2recomp::VU0_S2_VOPMULA),
                cop2Special1(kDestXYZ, 1, 2, 3, ps2recomp::VU0_S1_VOPMSUB),
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VMADDw),
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VMSUBq),
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VSUBi),
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VMULq),
                // No flags: MAX/MINI.
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VMAX),
                cop2Special1(kDestXYZW, 1, 2, 3, ps2recomp::VU0_S1_VMINIi),
                cfc2(2, ps2recomp::VU0_CR_MAC),
            };
            const std::string code = generate(ops);
            t.Equals(countOf(code, "PS2_VU0_FMAC_FLAGS"), size_t(11), "every FMAC op updates flags, MAX/MINI do not");
        });

        tc.Run("divide unit uses VU semantics and updates I/D when tracked", [](TestCase &t)
        {
            ps2recomp::R5900Decoder decoder;
            ps2recomp::CodeGenerator gen({}, {});
            // VRSQRT Q, vf1x, vf2y: fsf=x (L=0), ftf=y (M=1) -> dest bits = M<<2 | L.
            const auto rsqrt = decoder.decodeInstruction(0, cop2Special2(0x1u << 2, 2, 1, ps2recomp::VU0_S2_VRSQRT));
            const std::string untracked = gen.translateInstruction(rsqrt);
            t.IsTrue(untracked.find("Ps2Vu0Rsqrt(fs, ft") != std::string::npos, "VRSQRT must use fs / sqrt(|ft|)");
            t.IsTrue(untracked.find("ctx->vu0_vf[1]") != std::string::npos, "VRSQRT must read fs");
            t.IsTrue(untracked.find("Ps2Vu0SetDivFlags") == std::string::npos, "status untouched when not tracked");

            gen.m_vu0TrackFlags = true;
            const auto div = decoder.decodeInstruction(0, cop2Special2(0, 2, 1, ps2recomp::VU0_S2_VDIV));
            const auto sqrt = decoder.decodeInstruction(0, cop2Special2(0, 2, 0, ps2recomp::VU0_S2_VSQRT));
            t.IsTrue(gen.translateInstruction(div).find("Ps2Vu0Div(fs, ft, &divFlags); Ps2Vu0SetDivFlags") != std::string::npos,
                     "tracked VDIV sets I/D");
            t.IsTrue(gen.translateInstruction(sqrt).find("Ps2Vu0Sqrt(ft, &divFlags); Ps2Vu0SetDivFlags") != std::string::npos,
                     "tracked VSQRT sets I");
        });

        tc.Run("RSQRT.S computes fs / sqrt(ft)", [](TestCase &t)
        {
            ps2recomp::Instruction inst{};
            inst.opcode = ps2recomp::OPCODE_COP1;
            inst.rs = ps2recomp::COP1_S;
            inst.function = ps2recomp::COP1_S_RSQRT;
            inst.rd = 1; // fs
            inst.rt = 2; // ft
            inst.sa = 3; // fd
            ps2recomp::CodeGenerator gen({}, {});
            const std::string code = gen.translateInstruction(inst);
            t.IsTrue(code.find("ctx->f[3] = FPU_DIV_S(ctx->f[1], sqrtf(fabsf(ctx->f[2])))") != std::string::npos,
                     "RSQRT.S fd = fs / sqrt(|ft|)");
        });

        tc.Run("BC2 tests VU0 busy, not the status Z flag", [](TestCase &t)
        {
            // bc2t +1; nop
            const uint32_t bc2t = (0x12u << 26) | (ps2recomp::COP2_BC << 21) | (ps2recomp::COP2_BC_BCT << 16) | 1u;
            const std::string code = generate({bc2t, 0u, 0u});
            t.IsTrue(code.find("vu0_vpu_stat & 0x1") != std::string::npos, "BC2 should read VPU-STAT VBS0");
            t.IsTrue(code.find("vu0_status & 0x1") == std::string::npos, "BC2 must not read the Z flag");
        });

        tc.Run("runtime: MAC bits follow the dest layout (bit 3 = x)", [](TestCase &t)
        {
            R5900Context ctx{};
            // x = 0 (Z), y = -2 (S), z = denormal (Z+U), w = inf (O); all written.
            float denorm;
            const uint32_t denormBits = 0x00000010u;
            std::memcpy(&denorm, &denormBits, sizeof(denorm));
            Ps2Vu0UpdateFmacFlags(&ctx, vec(0.0f, -2.0f, denorm, INFINITY), 0xFu);
            t.Equals(ctx.vu0_mac_flags, 0x1000u | 0x0200u | 0x0040u | 0x000Au, "Zx|Zz, Sy, Uz, Ow");
            t.Equals(static_cast<uint32_t>(ctx.vu0_status), 0xFu | (0xFu << 6), "status Z/S/U/O and sticky copies");

            // Masked lanes contribute nothing and MAC is fully replaced; sticky bits accumulate.
            Ps2Vu0UpdateFmacFlags(&ctx, vec(1.0f, 0.0f, 0.0f, 0.0f), kDestX);
            t.Equals(ctx.vu0_mac_flags, 0u, "non-zero x with only x written clears MAC");
            t.Equals(static_cast<uint32_t>(ctx.vu0_status), 0xFu << 6, "Z/S/U/O cleared, sticky kept");

            Ps2Vu0UpdateFmacFlags(&ctx, vec(0.0f, 1.0f, 1.0f, 1.0f), kDestX);
            t.Equals(ctx.vu0_mac_flags, 0x8u, "zero length -> Zx, the bit the normalise helpers test");

            const uint32_t before = ctx.vu0_mac_flags;
            Ps2Vu0UpdateFmacFlags(&ctx, vec(5.0f, 5.0f, 5.0f, 5.0f), 0u);
            t.Equals(ctx.vu0_mac_flags, before, "dest 0 leaves flags untouched");
        });

        tc.Run("runtime: divide unit edge cases", [](TestCase &t)
        {
            uint32_t f = 0;
            t.Equals(Ps2Vu0Div(1.0f, 0.0f, &f), kFltMax, "x/+0 -> +max");
            t.Equals(f, 0x20u, "x/0 raises D");
            t.Equals(Ps2Vu0Div(1.0f, -0.0f, &f), -kFltMax, "x/-0 -> -max");
            t.Equals(Ps2Vu0Div(-0.0f, 0.0f, &f), -kFltMax, "0/0 sign from fs^ft");
            t.Equals(f, 0x10u, "0/0 raises I");
            t.Equals(Ps2Vu0Div(6.0f, 3.0f, &f), 2.0f, "normal divide");
            t.Equals(f, 0u, "no flags");

            t.Equals(Ps2Vu0Sqrt(-4.0f, &f), 2.0f, "sqrt uses |x|");
            t.Equals(f, 0x10u, "negative radicand raises I");

            t.Equals(Ps2Vu0Rsqrt(3.0f, 9.0f, &f), 1.0f, "rsqrt = fs / sqrt(ft)");
            t.Equals(Ps2Vu0Rsqrt(3.0f, -9.0f, &f), 1.0f, "rsqrt uses |ft|");
            t.Equals(f, 0x10u, "negative radicand raises I");
            t.Equals(Ps2Vu0Rsqrt(-3.0f, 0.0f, &f), -kFltMax, "rsqrt by zero -> max with sign of fs");
            t.Equals(f, 0x20u, "rsqrt by zero raises D");

            R5900Context ctx{};
            ctx.vu0_status = 0x0Fu;
            Ps2Vu0SetDivFlags(&ctx, 0x20u);
            Ps2Vu0SetDivFlags(&ctx, 0u);
            t.Equals(static_cast<uint32_t>(ctx.vu0_status), 0x0Fu | 0x800u, "D cleared by next op, DS sticky, FMAC bits kept");
        });
    });
}
