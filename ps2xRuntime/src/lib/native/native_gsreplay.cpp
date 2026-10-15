// Mode::Full: replays GS primitives (any GIF path, including VU1 XGKICK output of programs without a native
// port) as screen-space native batches, so the native frame contains everything drawn into its buffer, in the
// order the GS would draw it. Points and lines are counted but not drawn yet.
#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ps2native
{
    namespace
    {
        enum : uint32_t
        {
            PRIM_POINT = 0,
            PRIM_LINE = 1,
            PRIM_LINESTRIP = 2,
            PRIM_TRI = 3,
            PRIM_TRISTRIP = 4,
            PRIM_TRIFAN = 5,
            PRIM_SPRITE = 6,
        };

        struct GsVertex
        {
            float x = 0, y = 0, z = 0; // 12.4 window coordinates, raw Z
            uint32_t rgba = 0;
            float s = 0, t = 0, q = 1;
            uint32_t u = 0, v = 0; // 12.4 texel coordinates (FST)
        };

        struct PathState
        {
            uint64_t regs = 0;
            uint32_t fmt = 0, nreg = 1, reg = 0, loopsLeft = 0;
        };

        struct Replay
        {
            std::mutex mutex;
            bool (*source)(GSRawDrawRegisters &) = nullptr;
            std::array<PathState, 4> paths{};
            GSRawDrawRegisters regs{};
            uint64_t prim = 0;      // PRIM register
            uint64_t prmode = 0;    // PRMODE attributes (used when PRMODECONT.AC = 0)
            uint32_t rgba = 0x80808080u;
            float q = 1.0f;
            float s = 0.0f, t = 0.0f;
            uint32_t u = 0, v = 0;
            std::array<GsVertex, 3> queue{};
            uint32_t queued = 0;
            uint32_t stripParity = 0;
            Batch batch;
            bool batchOpen = false;
            bool skipping = false; // current primitive draws outside the native frame (render target, display copy)
            uint64_t unsupported = 0;
            // PS2X_NATIVE_REPLAYSTATS: primitives drawn / skipped per target page, and draws whose texture lies in a
            // page the GS rendered this session (the shadow never holds those pixels).
            uint64_t drawnPrims = 0, skippedPrims = 0, renderTargetSamples = 0;
            std::array<uint64_t, 512> pageRendered{}; // per 8 KB page: rendered by a replayed or skipped draw
            std::array<uint32_t, 512> skippedByPage{};

            uint64_t effectivePrim() const
            {
                const bool useAttributes = (regs.prmodecont & 1u) != 0;
                const uint64_t attributes = useAttributes ? prim : prmode;
                return (prim & 7u) | (attributes & 0x7f8u);
            }

            void flush()
            {
                if (batchOpen && !batch.indices.empty())
                    submit(std::move(batch));
                batch = Batch{};
                batchOpen = false;
            }

            // Starts a new batch when the drawing state changed; returns false for primitives not drawn natively.
            void openBatch(uint64_t p)
            {
                const unsigned ctx = unsigned(p >> 9) & 1u;
                const uint32_t fbp = uint32_t(regs.frame[ctx] & 0x1ffu);
                skipping = !isDrawBuffer(fbp);
                pageRendered[(fbp * 32u / 32u) & 511u] = 1;
                if (skipping)
                {
                    ++skippedPrims;
                    ++skippedByPage[fbp & 511u];
                    return;
                }
                ++drawnPrims;
                if ((p >> 4) & 1u)
                {
                    const uint32_t tbpPage = uint32_t(regs.tex0[ctx] & 0x3fffu) / 32u;
                    for (uint32_t page = 0; page < 512u; ++page)
                        if (pageRendered[page] && tbpPage >= page && tbpPage < page + 70u) // a 640x448x32 frame spans 70 pages
                        {
                            ++renderTargetSamples;
                            break;
                        }
                }
                if (batchOpen)
                {
                    const unsigned bctx = unsigned(batch.prim >> 9) & 1u;
                    const bool same = batch.prim == p && batch.regs.tex0[bctx] == regs.tex0[ctx] &&
                                      batch.regs.alpha[bctx] == regs.alpha[ctx] && batch.regs.test[bctx] == regs.test[ctx] &&
                                      batch.regs.frame[bctx] == regs.frame[ctx] && batch.regs.zbuf[bctx] == regs.zbuf[ctx] &&
                                      batch.regs.clamp[bctx] == regs.clamp[ctx] && batch.regs.tex1[bctx] == regs.tex1[ctx] &&
                                      batch.regs.xyoffset[bctx] == regs.xyoffset[ctx] && batch.regs.scissor[bctx] == regs.scissor[ctx] &&
                                      batch.regs.fba[bctx] == regs.fba[ctx] && batch.regs.texa == regs.texa &&
                                      batch.vertices.size() < 60000u;
                    if (same)
                        return;
                    flush();
                }
                batch.screenSpace = true;
                batch.prim = p;
                batch.regs = regs;
                batch.debugTag = 0xffffu;
                if ((p >> 4) & 1u)
                    batch.texture = requestTexture(regs.tex0[ctx], regs.texa);
                batchOpen = true;
            }

            Vertex toNative(const GsVertex &g, uint64_t p) const
            {
                Vertex out;
                out.pos[0] = g.x;
                out.pos[1] = g.y;
                out.pos[2] = g.z;
                out.rgba = g.rgba;
                if ((p >> 8) & 1u) // FST: UV in 12.4 texels
                {
                    const unsigned ctx = unsigned(p >> 9) & 1u;
                    const uint64_t tex0 = regs.tex0[ctx];
                    const float width = float(1u << std::min<uint32_t>(uint32_t((tex0 >> 26) & 15u), 10u));
                    const float height = float(1u << std::min<uint32_t>(uint32_t((tex0 >> 30) & 15u), 10u));
                    out.st[0] = float(g.u) / (16.0f * width);
                    out.st[1] = float(g.v) / (16.0f * height);
                    out.q = 1.0f;
                }
                else
                {
                    out.st[0] = g.s;
                    out.st[1] = g.t;
                    out.q = g.q;
                }
                return out;
            }

            void emitTriangle(const GsVertex &a, const GsVertex &b, const GsVertex &c, uint64_t p)
            {
                openBatch(p);
                if (skipping)
                    return;
                const bool flat = !((p >> 3) & 1u); // IIP = 0: the last vertex's colour
                const uint16_t first = uint16_t(batch.vertices.size());
                for (const GsVertex *g : {&a, &b, &c})
                {
                    Vertex v = toNative(*g, p);
                    if (flat)
                        v.rgba = c.rgba;
                    batch.vertices.push_back(v);
                }
                batch.indices.insert(batch.indices.end(), {first, uint16_t(first + 1u), uint16_t(first + 2u)});
            }

            void emitSprite(const GsVertex &a, const GsVertex &b, uint64_t p)
            {
                openBatch(p);
                if (skipping)
                    return;
                // Corners take x/y and texture coordinates from either vertex; colour and Z come from the second.
                Vertex v0 = toNative(a, p), v1 = toNative(b, p);
                // Sprites interpolate S/T/Q linearly; resolve to plain coordinates so corners can mix them.
                for (Vertex *v : {&v0, &v1})
                {
                    v->st[0] /= v->q;
                    v->st[1] /= v->q;
                    v->q = 1.0f;
                }
                const uint16_t first = uint16_t(batch.vertices.size());
                for (int corner = 0; corner < 4; ++corner)
                {
                    Vertex v = v1;
                    v.pos[0] = (corner & 1) ? v1.pos[0] : v0.pos[0];
                    v.pos[1] = (corner & 2) ? v1.pos[1] : v0.pos[1];
                    v.st[0] = (corner & 1) ? v1.st[0] : v0.st[0];
                    v.st[1] = (corner & 2) ? v1.st[1] : v0.st[1];
                    batch.vertices.push_back(v);
                }
                batch.indices.insert(batch.indices.end(),
                                     {first, uint16_t(first + 1u), uint16_t(first + 2u), uint16_t(first + 1u), uint16_t(first + 3u),
                                      uint16_t(first + 2u)});
            }

            // Vertex kick (XYZ2/XYZF2 draw, XYZ3/XYZF3 only queue).
            void kick(GsVertex vertex, bool draw)
            {
                vertex.rgba = rgba;
                vertex.s = s;
                vertex.t = t;
                vertex.q = q;
                vertex.u = u;
                vertex.v = v;
                const uint64_t p = effectivePrim();
                const uint32_t type = uint32_t(p & 7u);
                switch (type)
                {
                case PRIM_TRI:
                    queue[queued++] = vertex;
                    if (queued == 3)
                    {
                        if (draw)
                            emitTriangle(queue[0], queue[1], queue[2], p);
                        queued = 0;
                    }
                    break;
                case PRIM_TRISTRIP:
                    if (queued < 3)
                        queue[queued++] = vertex;
                    else
                    {
                        queue[0] = queue[1];
                        queue[1] = queue[2];
                        queue[2] = vertex;
                    }
                    if (queued == 3 && draw)
                        emitTriangle(queue[0], queue[1], queue[2], p);
                    break;
                case PRIM_TRIFAN:
                    if (queued < 3)
                        queue[queued++] = vertex;
                    else
                    {
                        queue[1] = queue[2];
                        queue[2] = vertex;
                    }
                    if (queued == 3 && draw)
                        emitTriangle(queue[0], queue[1], queue[2], p);
                    break;
                case PRIM_SPRITE:
                    queue[queued++] = vertex;
                    if (queued == 2)
                    {
                        if (draw)
                            emitSprite(queue[0], queue[1], p);
                        queued = 0;
                    }
                    break;
                default: // points and lines
                    ++unsupported;
                    queued = 0;
                    break;
                }
            }

            void writeRegister(uint32_t addr, uint64_t value)
            {
                switch (addr)
                {
                case 0x00: prim = value; queued = 0; break;
                case 0x01: rgba = uint32_t(value); std::memcpy(&q, reinterpret_cast<const uint8_t *>(&value) + 4, 4); break;
                case 0x02: std::memcpy(&s, &value, 4); std::memcpy(&t, reinterpret_cast<const uint8_t *>(&value) + 4, 4); break;
                case 0x03: u = uint32_t(value & 0x3fffu); v = uint32_t((value >> 16) & 0x3fffu); break;
                case 0x04: // XYZF2
                case 0x0c: // XYZF3
                {
                    GsVertex g;
                    g.x = float(value & 0xffffu);
                    g.y = float((value >> 16) & 0xffffu);
                    g.z = float((value >> 32) & 0xffffffu);
                    kick(g, addr == 0x04);
                    break;
                }
                case 0x05: // XYZ2
                case 0x0d: // XYZ3
                {
                    GsVertex g;
                    g.x = float(value & 0xffffu);
                    g.y = float((value >> 16) & 0xffffu);
                    g.z = float(uint32_t(value >> 32));
                    kick(g, addr == 0x05);
                    break;
                }
                case 0x06: regs.tex0[0] = value; break;
                case 0x07: regs.tex0[1] = value; break;
                case 0x08: regs.clamp[0] = value; break;
                case 0x09: regs.clamp[1] = value; break;
                case 0x14: regs.tex1[0] = value; break;
                case 0x15: regs.tex1[1] = value; break;
                case 0x16: // TEX2: CLUT fields of TEX0 plus PSM
                case 0x17:
                {
                    const uint64_t mask = (0x3full << 20) | (0x3fffull << 37) | (0xfull << 51) | (1ull << 55) | (0x1full << 56) | (7ull << 61);
                    uint64_t &tex0 = regs.tex0[addr - 0x16];
                    tex0 = (tex0 & ~mask) | (value & mask);
                    break;
                }
                case 0x18: regs.xyoffset[0] = value; break;
                case 0x19: regs.xyoffset[1] = value; break;
                case 0x1a: regs.prmodecont = value; break;
                case 0x1b: prmode = value; break;
                case 0x3b: regs.texa = value; break;
                case 0x3d: regs.fogcol = value; break;
                case 0x40: regs.scissor[0] = value; break;
                case 0x41: regs.scissor[1] = value; break;
                case 0x42: regs.alpha[0] = value; break;
                case 0x43: regs.alpha[1] = value; break;
                case 0x45: regs.dthe = value; break;
                case 0x46: regs.colclamp = value; break;
                case 0x47: regs.test[0] = value; break;
                case 0x48: regs.test[1] = value; break;
                case 0x49: regs.pabe = value; break;
                case 0x4a: regs.fba[0] = value; break;
                case 0x4b: regs.fba[1] = value; break;
                case 0x4c: regs.frame[0] = value; break;
                case 0x4d: regs.frame[1] = value; break;
                case 0x4e: regs.zbuf[0] = value; break;
                case 0x4f: regs.zbuf[1] = value; break;
                default: break;
                }
            }

            // PACKED register descriptor with its 128-bit payload.
            void packed(uint32_t desc, const uint8_t *qw)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, qw, 8);
                std::memcpy(&hi, qw + 8, 8);
                switch (desc)
                {
                case 0x0: writeRegister(0x00, lo); break;
                case 0x1: // RGBAQ: bytes in each 32-bit lane, Q from the last ST
                    rgba = uint32_t(lo & 0xffu) | uint32_t((lo >> 32) & 0xffu) << 8 | uint32_t(hi & 0xffu) << 16 |
                           uint32_t((hi >> 32) & 0xffu) << 24;
                    break;
                case 0x2: // ST, with Q in the third word
                    std::memcpy(&s, qw, 4);
                    std::memcpy(&t, qw + 4, 4);
                    std::memcpy(&q, qw + 8, 4);
                    break;
                case 0x3:
                    u = uint32_t(lo & 0x3fffu);
                    v = uint32_t((lo >> 32) & 0x3fffu);
                    break;
                case 0x4: // XYZF2 / ADC
                case 0x5: // XYZ2 / ADC
                {
                    GsVertex g;
                    g.x = float(lo & 0xffffu);
                    g.y = float((lo >> 32) & 0xffffu);
                    g.z = desc == 0x4 ? float((hi >> 4) & 0xffffffu) : float(uint32_t(hi));
                    const bool adc = (hi >> 47) & 1u;
                    kick(g, !adc);
                    break;
                }
                case 0xc:
                case 0xd:
                {
                    GsVertex g;
                    g.x = float(lo & 0xffffu);
                    g.y = float((lo >> 32) & 0xffffu);
                    g.z = desc == 0xc ? float((hi >> 4) & 0xffffffu) : float(uint32_t(hi));
                    kick(g, false);
                    break;
                }
                case 0x6: regs.tex0[0] = lo; break;
                case 0x7: regs.tex0[1] = lo; break;
                case 0x8: regs.clamp[0] = lo; break;
                case 0x9: regs.clamp[1] = lo; break;
                case 0xa: break; // FOG
                case 0xe: writeRegister(uint32_t(hi & 0xffu), lo); break; // A+D
                default: break;
                }
            }

            void gif(uint32_t pathIndex, const uint8_t *data, uint32_t size)
            {
                PathState &p = paths[pathIndex & 3u];
                uint32_t off = 0;
                while (off + 8u <= size)
                {
                    if (p.loopsLeft == 0)
                    {
                        if (off + 16u > size)
                            break;
                        uint64_t tag[2];
                        std::memcpy(tag, data + off, 16);
                        off += 16u;
                        p.loopsLeft = uint32_t(tag[0] & 0x7fffu);
                        p.fmt = uint32_t((tag[0] >> 58) & 3u);
                        p.nreg = ((tag[0] >> 60) & 15u) ? uint32_t((tag[0] >> 60) & 15u) : 16u;
                        p.regs = tag[1];
                        p.reg = 0;
                        if (((tag[0] >> 46) & 1u) && p.fmt != 2u) // PRE: PRIM from the tag
                            writeRegister(0x00, (tag[0] >> 47) & 0x7ffu);
                        continue;
                    }
                    if (p.fmt == 0u)
                    {
                        if (off + 16u > size)
                            break;
                        packed(uint32_t((p.regs >> (p.reg * 4u)) & 15u), data + off);
                        off += 16u;
                        if (++p.reg == p.nreg)
                        {
                            p.reg = 0;
                            --p.loopsLeft;
                        }
                    }
                    else if (p.fmt == 1u) // REGLIST: 64-bit register values
                    {
                        const uint32_t desc = uint32_t((p.regs >> (p.reg * 4u)) & 15u);
                        uint64_t value;
                        std::memcpy(&value, data + off, 8);
                        static const uint32_t addresses[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                                               0x08, 0x09, 0x0a, 0xff, 0x0c, 0x0d, 0xff, 0xff};
                        if (addresses[desc] != 0xffu)
                            writeRegister(addresses[desc], value);
                        off += 8u;
                        if (++p.reg == p.nreg)
                        {
                            p.reg = 0;
                            if (--p.loopsLeft == 0 && (off & 15u))
                                off += 8u;
                        }
                    }
                    else // IMAGE / disabled: payload only
                    {
                        const uint32_t count = std::min(p.loopsLeft, (size - off) / 16u);
                        if (count == 0)
                            break;
                        p.loopsLeft -= count;
                        off += count * 16u;
                    }
                }
            }
        };

        Replay &replay()
        {
            static Replay r;
            return r;
        }
    }

    void setRegisterSource(bool (*source)(GSRawDrawRegisters &out))
    {
        auto &r = replay();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.source = source;
    }

    void replayGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (mode() != Mode::Full || !data || sizeBytes < 16u)
            return;
        auto &r = replay();
        std::lock_guard<std::mutex> lock(r.mutex);
        // Context registers can also change outside GIF packets (privileged writes); start from the GS's view.
        if (r.source)
        {
            GSRawDrawRegisters current;
            if (r.source(current))
            {
                r.regs = current;
                r.prim = current.prim;
            }
        }
        r.gif(path, data, sizeBytes);
        r.flush();
        static const bool stats = std::getenv("PS2X_NATIVE_REPLAYSTATS") != nullptr;
        static auto last = std::chrono::steady_clock::now();
        if (stats && std::chrono::steady_clock::now() - last > std::chrono::seconds(5))
        {
            last = std::chrono::steady_clock::now();
            std::string pages;
            for (uint32_t page = 0; page < 512u; ++page)
                if (r.skippedByPage[page])
                    pages += " " + std::to_string(page) + ":" + std::to_string(r.skippedByPage[page]);
            std::fprintf(stderr, "[native-replay] drawn %llu, skipped %llu (fbp:count%s), points/lines %llu, render-target samples %llu\n",
                         (unsigned long long)r.drawnPrims, (unsigned long long)r.skippedPrims, pages.c_str(),
                         (unsigned long long)r.unsupported, (unsigned long long)r.renderTargetSamples);
            r.drawnPrims = r.skippedPrims = r.unsupported = r.renderTargetSamples = 0;
            r.skippedByPage.fill(0);
        }
    }
}
