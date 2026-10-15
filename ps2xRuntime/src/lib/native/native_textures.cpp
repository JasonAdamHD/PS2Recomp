// Texture side of the native renderer: a CPU shadow of GS local memory fed by the GIF transfers the game makes,
// and RGBA8 decodes of the textures native batches sample. Textures the GS renders itself (render targets)
// never reach the shadow; such batches see stale data until that case is handled.
#include "runtime/native/native_renderer.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2native
{
    namespace
    {
        constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
        constexpr uint32_t kPageCount = kVramSize / 8192u;

        enum : uint32_t
        {
            PSMCT32 = 0x00,
            PSMCT24 = 0x01,
            PSMCT16 = 0x02,
            PSMCT16S = 0x0a,
            PSMT8 = 0x13,
            PSMT4 = 0x14,
            PSMT8H = 0x1b,
            PSMT4HL = 0x24,
            PSMT4HH = 0x2c,
            PSMZ32 = 0x30,
            PSMZ24 = 0x31,
            PSMZ16 = 0x32,
            PSMZ16S = 0x3a,
        };

        using Writer = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

        struct Format
        {
            Writer write = nullptr;
            Reader read = nullptr;
            uint32_t bits = 0;
            uint32_t pageWidth = 64, pageHeight = 32;
            uint32_t blockWidth = 8, blockHeight = 8;
        };

        Format format(uint32_t psm)
        {
            using namespace GSMem;
            switch (psm)
            {
            case PSMCT32: return {WriteCT32, ReadCT32, 32, 64, 32, 8, 8};
            case PSMCT24: return {WriteCT24, ReadCT24, 24, 64, 32, 8, 8};
            case PSMCT16: return {WriteCT16, ReadCT16, 16, 64, 64, 16, 8};
            case PSMCT16S: return {WriteCT16S, ReadCT16S, 16, 64, 64, 16, 8};
            case PSMT8: return {WriteP8, ReadP8, 8, 128, 64, 16, 16};
            case PSMT4: return {WriteP4, ReadP4, 4, 128, 128, 32, 16};
            case PSMT8H: return {WriteP8H, ReadP8H, 8, 64, 32, 8, 8};
            case PSMT4HL: return {WriteP4HL, ReadP4HL, 4, 64, 32, 8, 8};
            case PSMT4HH: return {WriteP4HH, ReadP4HH, 4, 64, 32, 8, 8};
            case PSMZ32: return {WriteZ32, ReadZ32, 32, 64, 32, 8, 8};
            case PSMZ24: return {WriteZ24, ReadZ24, 24, 64, 32, 8, 8};
            case PSMZ16: return {WriteZ16, ReadZ16, 16, 64, 64, 16, 8};
            case PSMZ16S: return {WriteZ16S, ReadZ16S, 16, 64, 64, 16, 8};
            default: return {};
            }
        }

        struct GifPath
        {
            uint64_t regs = 0;
            uint32_t fmt = 0, nreg = 1, reg = 0, loopsLeft = 0;
        };

        struct Shadow
        {
            std::mutex mutex;
            std::vector<uint8_t> vram = std::vector<uint8_t>(kVramSize, 0u);
            std::array<uint64_t, kPageCount> pageGeneration{};
            uint64_t generation = 0;
            std::array<GifPath, 4> paths{};

            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            // Host->local transfer in progress.
            bool uploading = false;
            Format dst{};
            uint32_t dpsm = 0;
            uint32_t dbp = 0, dbw = 0, dsax = 0, dsay = 0, rrw = 0, rrh = 0, x = 0, y = 0;
            uint32_t pending = 0, pendingBits = 0; // bits carried between IMAGE chunks

            struct CacheEntry
            {
                uint64_t texSignature = 0, palSignature = 0; // page generations when texture was looked up
                std::shared_ptr<const Texture> texture;
            };
            std::unordered_map<uint64_t, CacheEntry> cache;                       // by TEX0 (fast path)
            std::unordered_map<uint64_t, std::shared_ptr<const Texture>> byContent; // by contents
            // PS2X_NATIVE_TEXSTATS: lookups, texel re-reads (and texels read), palette changes, sizes re-read.
            uint64_t lookups = 0, rereads = 0, texelsRead = 0, paletteChanges = 0, uploads = 0, uploadPixels = 0;
            std::unordered_map<uint64_t, uint64_t> rereadByKey;
            uint64_t serial = 0;

            // Marks every page a rectangle of the buffer at bp touches (once per transfer, not per pixel).
            void touchRect(uint32_t bp, uint32_t bw, const Format &f, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
            {
                if (!w || !h)
                    return;
                const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u) / f.pageWidth);
                for (uint32_t py = y0 / f.pageHeight; py <= (y0 + h - 1u) / f.pageHeight; ++py)
                    for (uint32_t px = x0 / f.pageWidth; px <= (x0 + w - 1u) / f.pageWidth + (bp % 32u ? 1u : 0u); ++px)
                        pageGeneration[(bp / 32u + py * pagesPerRow + px) % kPageCount] = generation;
            }

            // Pixels are gathered per row and written with GSMem::WriteRow (inlined swizzle).
            std::array<uint32_t, 2048> row{};
            uint32_t rowFill = 0, rowStart = 0;

            void flushRow()
            {
                if (rowFill)
                    GSMem::WriteRow(vram.data(), dpsm, dbp, dbw, (dsax + rowStart) & 2047u, (dsay + y) & 2047u, row.data(), rowFill);
                rowFill = 0;
            }

            void writePixel(uint32_t value)
            {
                if (rowFill == 0)
                    rowStart = x;
                row[rowFill++] = value;
                if (++x == rrw)
                {
                    flushRow();
                    x = 0;
                    if (++y == rrh)
                        uploading = false;
                }
            }

            void image(const uint8_t *data, uint32_t size)
            {
                if (!uploading || !dst.write)
                    return;
                uint32_t i = 0;
                // Whole pixels straight from the stream when no partial pixel is pending.
                if (pendingBits == 0 && (dst.bits == 32 || dst.bits == 16 || dst.bits == 8))
                {
                    const uint32_t step = dst.bits / 8u;
                    for (; i + step <= size && uploading; i += step)
                    {
                        uint32_t value = 0;
                        std::memcpy(&value, data + i, step);
                        writePixel(value);
                    }
                }
                for (; i < size && uploading; ++i)
                {
                    if (dst.bits == 4)
                    {
                        writePixel(data[i] & 15u);
                        if (uploading)
                            writePixel(data[i] >> 4);
                        continue;
                    }
                    pending |= uint32_t(data[i]) << pendingBits;
                    pendingBits += 8;
                    if (pendingBits == dst.bits)
                    {
                        writePixel(pending);
                        pending = pendingBits = 0;
                    }
                }
                flushRow();
            }

            void startTransfer(uint32_t dir)
            {
                uploading = false;
                pending = pendingBits = 0;
                ++generation;
                rrw = uint32_t(trxreg & 0xfffu);
                rrh = uint32_t((trxreg >> 32) & 0xfffu);
                if (dir == 0u)
                {
                    dbp = uint32_t((bitbltbuf >> 32) & 0x3fffu);
                    dbw = uint32_t((bitbltbuf >> 48) & 0x3fu);
                    dpsm = uint32_t((bitbltbuf >> 56) & 0x3fu);
                    dst = format(dpsm);
                    rowFill = 0;
                    dsax = uint32_t((trxpos >> 32) & 0x7ffu);
                    dsay = uint32_t((trxpos >> 48) & 0x7ffu);
                    x = y = 0;
                    uploading = dst.write && rrw && rrh;
                    ++uploads;
                    uploadPixels += uint64_t(rrw) * rrh;
                    if (uploading)
                        touchRect(dbp, dbw, dst, dsax, dsay, rrw, rrh);
                }
                else if (dir == 2u)
                {
                    // Local->local copy (DIR ordering is ignored; overlapping copies are rare).
                    const Format src = format(uint32_t((bitbltbuf >> 24) & 0x3fu));
                    const Format to = format(uint32_t((bitbltbuf >> 56) & 0x3fu));
                    if (!src.read || !to.write)
                        return;
                    const uint32_t sbp = uint32_t(bitbltbuf & 0x3fffu), sbw = uint32_t((bitbltbuf >> 16) & 0x3fu);
                    const uint32_t tbp = uint32_t((bitbltbuf >> 32) & 0x3fffu), tbw = uint32_t((bitbltbuf >> 48) & 0x3fu);
                    const uint32_t ssax = uint32_t(trxpos & 0x7ffu), ssay = uint32_t((trxpos >> 16) & 0x7ffu);
                    const uint32_t tsax = uint32_t((trxpos >> 32) & 0x7ffu), tsay = uint32_t((trxpos >> 48) & 0x7ffu);
                    for (uint32_t row = 0; row < rrh; ++row)
                        for (uint32_t col = 0; col < rrw; ++col)
                        {
                            const uint32_t v = src.read(vram.data(), sbp, sbw, (ssax + col) & 2047u, (ssay + row) & 2047u);
                            to.write(vram.data(), tbp, tbw, (tsax + col) & 2047u, (tsay + row) & 2047u, v);
                        }
                    touchRect(tbp, tbw, to, tsax, tsay, rrw, rrh);
                }
            }

            void writeRegister(uint32_t addr, uint64_t value)
            {
                switch (addr)
                {
                case 0x50: bitbltbuf = value; break;
                case 0x51: trxpos = value; break;
                case 0x52: trxreg = value; break;
                case 0x53: startTransfer(uint32_t(value & 3u)); break;
                case 0x54: // HWREG: image data written as a register, 8 bytes at a time
                {
                    uint8_t bytes[8];
                    std::memcpy(bytes, &value, 8);
                    image(bytes, 8u);
                    break;
                }
                default: break;
                }
            }

            void gif(uint32_t pathIndex, const uint8_t *data, uint32_t size)
            {
                GifPath &p = paths[pathIndex & 3u];
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
                        continue;
                    }
                    if (p.fmt == 0u) // PACKED
                    {
                        if (off + 16u > size)
                            break;
                        if (((p.regs >> (p.reg * 4u)) & 15u) == 0xeu) // A+D
                        {
                            uint64_t q[2];
                            std::memcpy(q, data + off, 16);
                            writeRegister(uint32_t(q[1] & 0xffu), q[0]);
                        }
                        off += 16u;
                        if (++p.reg == p.nreg)
                        {
                            p.reg = 0;
                            --p.loopsLeft;
                        }
                    }
                    else if (p.fmt == 1u) // REGLIST: no A+D, just skip the payload
                    {
                        off += 8u;
                        if (++p.reg == p.nreg)
                        {
                            p.reg = 0;
                            if (--p.loopsLeft == 0 && (off & 15u))
                                off += 8u;
                        }
                    }
                    else // IMAGE
                    {
                        const uint32_t count = std::min(p.loopsLeft, (size - off) / 16u);
                        if (count == 0)
                            break;
                        image(data + off, count * 16u);
                        p.loopsLeft -= count;
                        off += count * 16u;
                    }
                }
            }

            // The 256-byte blocks a texture occupies, found once per texture shape by writing one pixel per block
            // into a scratch copy of local memory (the swizzle tables are private to GSMem).
            std::vector<uint8_t> probe;
            std::unordered_map<uint64_t, std::vector<uint32_t>> blockLists;

            const std::vector<uint32_t> &blocksOf(uint64_t shapeKey, uint32_t bp, uint32_t bw, const Format &f, uint32_t w, uint32_t h)
            {
                auto it = blockLists.find(shapeKey);
                if (it != blockLists.end())
                    return it->second;
                if (probe.empty())
                    probe.assign(kVramSize, 0u);
                std::vector<uint32_t> blocks;
                for (uint32_t y = 0; y < h; y += f.blockHeight)
                    for (uint32_t x = 0; x < w; x += f.blockWidth)
                    {
                        f.write(probe.data(), bp, bw, x, y, ~0u);
                        // The pixel lands within the texture's page span after bp; search it word by word.
                        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (bw * 64u) / f.pageWidth);
                        const uint32_t span = ((y / f.pageHeight) * pagesPerRow + x / f.pageWidth + 2u) * 8192u;
                        for (uint32_t off = 0; off < span; off += 8u)
                        {
                            const uint32_t at = (bp * 256u + off) % kVramSize;
                            uint64_t word;
                            std::memcpy(&word, probe.data() + at, 8);
                            if (word)
                            {
                                blocks.push_back(at / 256u);
                                std::memset(probe.data() + at, 0, 8);
                                break;
                            }
                        }
                    }
                std::sort(blocks.begin(), blocks.end());
                blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
                if (blockLists.size() > 65536u)
                    blockLists.clear();
                return blockLists[shapeKey] = std::move(blocks);
            }

            uint64_t blocksContent(const std::vector<uint32_t> &blocks) const
            {
                uint64_t hash = 0x9E3779B97F4A7C15ull;
                for (uint32_t block : blocks)
                {
                    const uint8_t *bytes = vram.data() + size_t(block) * 256u;
                    for (uint32_t i = 0; i < 256u; i += 8u)
                    {
                        uint64_t word;
                        std::memcpy(&word, bytes + i, 8);
                        hash = (hash ^ word) * 0x100000001b3ull;
                        hash ^= hash >> 29;
                    }
                }
                return hash;
            }

            uint64_t pagesSignature(uint32_t bp, uint32_t bw, const Format &f, uint32_t w, uint32_t h) const
            {
                const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bw, 1u) * 64u) / f.pageWidth);
                const uint32_t cols = std::max<uint32_t>(1u, (w + f.pageWidth - 1u) / f.pageWidth);
                const uint32_t rows = std::max<uint32_t>(1u, (h + f.pageHeight - 1u) / f.pageHeight);
                uint64_t sig = 0;
                for (uint32_t r = 0; r < rows; ++r)
                    for (uint32_t c = 0; c <= cols; ++c) // one extra column covers block-misaligned bases
                    {
                        const uint32_t page = (bp / 32u + r * pagesPerRow + c) % kPageCount;
                        sig = sig * 0x100000001b3ull ^ pageGeneration[page];
                    }
                return sig;
            }
        };

        Shadow &shadow()
        {
            static Shadow s;
            return s;
        }

        uint32_t expand16(uint32_t c, uint64_t texa)
        {
            const uint32_t r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
            const bool aem = (texa >> 15) & 1u;
            uint32_t a;
            if (c & 0x8000u)
                a = uint32_t((texa >> 32) & 0xffu);
            else
                a = (aem && (c & 0x7fffu) == 0) ? 0u : uint32_t(texa & 0xffu);
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        uint32_t expand24(uint32_t c, uint64_t texa)
        {
            const bool aem = (texa >> 15) & 1u;
            const uint32_t a = (aem && (c & 0xffffffu) == 0) ? 0u : uint32_t(texa & 0xffu);
            return (c & 0xffffffu) | (a << 24);
        }
    }

    namespace
    {
    std::shared_ptr<const Texture> decodeTexture(uint64_t tex0, uint64_t texa)
    {
        const uint32_t tbp = uint32_t(tex0 & 0x3fffu), tbw = uint32_t((tex0 >> 14) & 0x3fu);
        const uint32_t psm = uint32_t((tex0 >> 20) & 0x3fu);
        const uint32_t tw = std::min<uint32_t>(uint32_t((tex0 >> 26) & 15u), 10u);
        const uint32_t th = std::min<uint32_t>(uint32_t((tex0 >> 30) & 15u), 10u);
        const uint32_t cbp = uint32_t((tex0 >> 37) & 0x3fffu), cpsm = uint32_t((tex0 >> 51) & 15u);
        const Format f = format(psm);
        if (!f.read)
            return nullptr;
        const bool indexed = psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
        const Format cf = format(cpsm == 0u ? PSMCT32 : (cpsm == 0xau ? PSMCT16S : PSMCT16));
        const uint32_t width = 1u << tw, height = 1u << th;

        // Key: everything that selects the decoded texels.
        uint64_t key = tex0 & ((1ull << 34) - 1ull); // TBP0, TBW, PSM, TW, TH
        if (indexed)
            key ^= (tex0 & (0x3fffull << 37 | 0xfull << 51)) * 0x9E3779B97F4A7C15ull;
        if (psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S || (indexed && cpsm != 0u))
            key ^= (texa & 0xff000080ffull) * 0xC2B2AE3D27D4EB4Full;

        auto &s = shadow();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.cache.size() > 8192u)
            s.cache.clear();
        ++s.lookups;
        static const bool texStats = std::getenv("PS2X_NATIVE_TEXSTATS") != nullptr;
        if (texStats)
        {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last).count() >= 5.0)
            {
                std::fprintf(stderr, "[native-tex] lookups %llu rereads %llu texels %llu (palette changes %llu) uploads %llu (%llu px) cache %zu keys rereading %zu\n",
                             (unsigned long long)s.lookups, (unsigned long long)s.rereads, (unsigned long long)s.texelsRead,
                             (unsigned long long)s.paletteChanges, (unsigned long long)s.uploads, (unsigned long long)s.uploadPixels,
                             s.cache.size(), s.rereadByKey.size());
                for (const auto &kv : s.rereadByKey)
                    if (kv.second > 50)
                        std::fprintf(stderr, "[native-tex]   tex0 low 0x%llx rereads %llu\n", (unsigned long long)kv.first, (unsigned long long)kv.second);
                s.lookups = s.rereads = s.texelsRead = s.paletteChanges = s.uploads = s.uploadPixels = 0;
                s.rereadByKey.clear();
                last = now;
            }
        }
        auto &entry = s.cache[key];
        const uint64_t texSignature = s.pagesSignature(tbp, tbw, f, width, height);
        const uint64_t palSignature = indexed ? s.pagesSignature(cbp, 1u, cf, 16u, 16u) : 0u;
        if (entry.texture && entry.texSignature == texSignature && entry.palSignature == palSignature)
            return entry.texture;

        // The game's VRAM cache swaps textures through the same addresses all frame, so decoded textures are kept
        // by content: the hash of the texture's own blocks, its format and its palette.
        const uint64_t content =
            s.blocksContent(s.blocksOf(tex0 & ((1ull << 34) - 1ull), tbp, std::max<uint32_t>(tbw, 1u), f, width, height));

        // Palette (CSM1): T8 entries live in a 16x16 block with index bits 3 and 4 swapped, T4 in 8x2.
        std::array<uint32_t, 256> palette{};
        uint64_t paletteHash = 0;
        if (indexed)
        {
            const uint32_t entries = f.bits == 8 ? 256u : 16u;
            paletteHash = 0x9E3779B97F4A7C15ull;
            for (uint32_t i = 0; i < entries; ++i)
            {
                uint32_t px, py;
                if (entries == 256u)
                {
                    const uint32_t j = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                    px = j & 15u;
                    py = j >> 4;
                }
                else
                {
                    px = i & 7u;
                    py = i >> 3;
                }
                const uint32_t c = cf.read(s.vram.data(), cbp, 1u, px, py);
                palette[i] = cf.bits == 32 ? c : expand16(c, texa);
                paletteHash = (paletteHash ^ palette[i]) * 0x100000001b3ull;
            }
        }
        // Format bits without TBP0: TBW, PSM, TW, TH.
        const bool usesTexa = psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S || (indexed && cpsm != 0u);
        const uint64_t contentKey = (content * 0x9E3779B97F4A7C15ull) ^ (((tex0 >> 14) & 0xfffffull) * 0x165667B19E3779F9ull) ^
                                    (paletteHash * 0xC2B2AE3D27D4EB4Full) ^ (usesTexa ? (texa & 0xff000080ffull) * 0x27D4EB2F165667C5ull : 0u);
        entry.texSignature = texSignature;
        entry.palSignature = palSignature;
        auto found = s.byContent.find(contentKey);
        if (found != s.byContent.end())
        {
            entry.texture = found->second;
            return entry.texture;
        }

        auto texture = std::make_shared<Texture>();
        texture->serial = ++s.serial;
        texture->width = width;
        texture->height = height;
        texture->rgba.resize(size_t(width) * height);
        const uint32_t bw = std::max<uint32_t>(tbw, 1u);
        std::vector<uint32_t> texels(width);
        for (uint32_t v = 0; v < height; ++v)
        {
            GSMem::ReadRow(s.vram.data(), psm, tbp, bw, 0u, v, texels.data(), width);
            for (uint32_t u = 0; u < width; ++u)
            {
                const uint32_t c = texels[u];
                uint32_t out;
                if (indexed)
                    out = palette[c & 255u];
                else if (f.bits == 16)
                    out = expand16(c, texa);
                else if (f.bits == 24)
                    out = expand24(c, texa);
                else
                    out = c;
                texture->rgba[size_t(v) * width + u] = out;
            }
        }
        ++s.rereads;
        texture->black = std::all_of(texture->rgba.begin(), texture->rgba.end(), [](uint32_t c) { return (c & 0x00ffffffu) == 0u; });
        if (texStats)
        {
            bool allZero = true;
            for (uint32_t c : texture->rgba)
                if (c & 0x00ffffffu)
                {
                    allZero = false;
                    break;
                }
            static uint32_t reported = 0;
            if (allZero && reported < 40)
            {
                ++reported;
                const uint32_t page = tbp / 32u;
                std::fprintf(stderr, "[native-tex] black texture tex0 0x%016llx (tbp 0x%x psm 0x%x %ux%u cbp 0x%x) page gen %llu clut page gen %llu\n",
                             (unsigned long long)tex0, tbp, psm, width, height, cbp,
                             (unsigned long long)s.pageGeneration[page % kPageCount],
                             (unsigned long long)s.pageGeneration[(cbp / 32u) % kPageCount]);
            }
        }
        s.texelsRead += texture->rgba.size();
        ++s.rereadByKey[tex0 & ((1ull << 34) - 1ull)];
        if (s.byContent.size() > 16384u)
            s.byContent.clear();
        s.byContent[contentKey] = texture;
        entry.texture = texture;
        return texture;
    }

    // Work for the texture thread, in GS stream order.
    struct Command
    {
        uint32_t path = 0;
        std::vector<uint8_t> packet;
        std::shared_ptr<TextureRequest> request;
        uint64_t tex0 = 0, texa = 0;
    };

    struct Queue
    {
        std::mutex mutex;
        std::condition_variable wake, space;
        std::deque<Command> commands;
        size_t bytes = 0;
        bool started = false;
    };

    Queue &queue()
    {
        static Queue q;
        return q;
    }

    void textureThread()
    {
        auto &q = queue();
        for (;;)
        {
            Command command;
            {
                std::unique_lock<std::mutex> lock(q.mutex);
                q.wake.wait(lock, [&] { return !q.commands.empty(); });
                command = std::move(q.commands.front());
                q.commands.pop_front();
                q.bytes -= command.packet.size();
            }
            q.space.notify_one();
            if (command.request)
                command.request->resolve(decodeTexture(command.tex0, command.texa));
            else
            {
                auto &s = shadow();
                std::lock_guard<std::mutex> lock(s.mutex);
                s.gif(command.path, command.packet.data(), uint32_t(command.packet.size()));
            }
        }
    }

    void push(Command &&command)
    {
        auto &q = queue();
        {
            std::unique_lock<std::mutex> lock(q.mutex);
            if (!q.started)
            {
                q.started = true;
                std::thread(textureThread).detach();
            }
            // Back-pressure: the graphics thread waits if the texture thread falls far behind.
            q.space.wait(lock, [&] { return q.bytes < (64u << 20); });
            q.bytes += command.packet.size();
            q.commands.push_back(std::move(command));
        }
        q.wake.notify_one();
    }
    }

    std::shared_ptr<const Texture> TextureRequest::wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return done_; });
        return texture_;
    }

    void TextureRequest::resolve(std::shared_ptr<const Texture> texture)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            texture_ = std::move(texture);
            done_ = true;
        }
        ready_.notify_all();
    }

    void observeGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return;
        Command command;
        command.path = path;
        command.packet.assign(data, data + sizeBytes);
        push(std::move(command));
    }

    uint32_t debugShadowRead(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        auto request = requestTexture(0, 0); // resolves once everything queued before it has been applied
        request->wait();
        auto &s = shadow();
        std::lock_guard<std::mutex> lock(s.mutex);
        uint32_t value = 0;
        GSMem::ReadRow(s.vram.data(), psm, bp, std::max<uint32_t>(bw, 1u), x, y, &value, 1u);
        return value;
    }

    std::shared_ptr<TextureRequest> requestTexture(uint64_t tex0, uint64_t texa)
    {
        Command command;
        command.request = std::make_shared<TextureRequest>();
        command.tex0 = tex0;
        command.texa = texa;
        auto request = command.request;
        push(std::move(command));
        return request;
    }
}
