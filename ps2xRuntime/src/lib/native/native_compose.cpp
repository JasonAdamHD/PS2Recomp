#include "native_compose.h"
#include "runtime/native/native_renderer.h"

#include "device.hpp"
#include "command_buffer.hpp"

#include "native3d_vert.h"
#include "native3d_frag.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ps2native
{
    namespace
    {
        struct PushConstants
        {
            float objectToClip[16];
            float screen[4];
            float target[4];
            float misc[4];
            float shade[4];
        };
        static_assert(sizeof(PushConstants) == 128, "push constants must match native3d.vert/frag");

        uint32_t debugTint(uint32_t tag)
        {
            uint32_t h = tag * 0x9E3779B1u;
            h ^= h >> 15;
            return (h & 0x00FFFFFFu) | 0xFF000000u;
        }

        float depthRange(uint64_t zbuf)
        {
            switch ((zbuf >> 24) & 15u)
            {
            case 0: // PSMZ32
                return 4294967296.0f;
            case 1: // PSMZ24
                return 16777216.0f;
            default: // PSMZ16 / PSMZ16S
                return 65536.0f;
            }
        }

        // GS blending: ((A - B) * C >> 7) + D with A, B, D in {Cs, Cd, 0} and C in {As, Ad, FIX}.
        // Maps the combinations a fixed-function blender can express; anything else draws opaque.
        void applyBlend(Vulkan::CommandBuffer &cmd, uint64_t alpha)
        {
            const unsigned a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
            VkBlendFactor factor = c == 0 ? VK_BLEND_FACTOR_SRC_ALPHA : (c == 1 ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_CONSTANT_ALPHA);
            VkBlendFactor inverse = c == 0 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                           : (c == 1 ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA);
            const float fix = float((alpha >> 32) & 0xffu) / 128.0f;
            const float constants[4] = {fix, fix, fix, fix};
            cmd.set_blend_constants(constants);

            VkBlendFactor src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO;
            VkBlendOp op = VK_BLEND_OP_ADD;
            enum
            {
                Cs = 0,
                Cd = 1,
                Zero = 2
            };
            if (a == b)
            {
                // Just D.
                src = d == Cs ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
                dst = d == Cd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            }
            else if (a == Cs && b == Cd && d == Cd)
            {
                src = factor;
                dst = inverse;
            }
            else if (a == Cd && b == Cs && d == Cs)
            {
                src = inverse;
                dst = factor;
            }
            else if (a == Cs && b == Zero && d == Cd)
            {
                src = factor;
                dst = VK_BLEND_FACTOR_ONE;
            }
            else if (a == Cs && b == Zero && d == Zero)
            {
                src = factor;
                dst = VK_BLEND_FACTOR_ZERO;
            }
            else if (a == Zero && b == Cs && d == Cd)
            {
                src = factor;
                dst = VK_BLEND_FACTOR_ONE;
                op = VK_BLEND_OP_REVERSE_SUBTRACT;
            }
            else if (a == Cd && b == Zero && d == Cs)
            {
                src = VK_BLEND_FACTOR_ONE;
                dst = factor;
            }
            else
            {
                cmd.set_blend_enable(false);
                return;
            }
            cmd.set_blend_enable(true);
            // The GS never blends alpha: the framebuffer gets As (stored as As / 0x80, the scale DST_ALPHA expects).
            cmd.set_blend_factors(src, VK_BLEND_FACTOR_ONE, dst, VK_BLEND_FACTOR_ZERO);
            cmd.set_blend_op(op, VK_BLEND_OP_ADD);
        }

        struct GpuTexture
        {
            Vulkan::ImageHandle image;
            uint64_t lastUse = 0;
        };

        struct Cache
        {
            std::unordered_map<uint64_t, GpuTexture> textures;
            Vulkan::ImageHandle white;
            uint64_t frame = 0;
        };

        Cache &cache()
        {
            static Cache c;
            return c;
        }

        const Vulkan::ImageView &textureView(Vulkan::Device &device, const Texture *texture)
        {
            auto &c = cache();
            if (!c.white)
            {
                const uint32_t white = 0xffffffffu;
                Vulkan::ImageInitialData init{&white, 0, 0};
                c.white = device.create_image(Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM), &init);
            }
            if (!texture || texture->rgba.empty())
                return c.white->get_view();
            auto &entry = c.textures[texture->serial];
            if (!entry.image)
            {
                Vulkan::ImageInitialData init{texture->rgba.data(), 0, 0};
                entry.image = device.create_image(
                    Vulkan::ImageCreateInfo::immutable_2d_image(texture->width, texture->height, VK_FORMAT_R8G8B8A8_UNORM), &init);
                if (!entry.image)
                {
                    c.textures.erase(texture->serial);
                    return c.white->get_view();
                }
            }
            entry.lastUse = c.frame;
            return entry.image->get_view();
        }

        void evictTextures()
        {
            auto &c = cache();
            ++c.frame;
            for (auto it = c.textures.begin(); it != c.textures.end();)
                it = (c.frame - it->second.lastUse > 120u) ? c.textures.erase(it) : std::next(it);
        }

        Vulkan::StockSampler sampler(uint64_t tex1, uint64_t clamp)
        {
            const bool linear = (tex1 >> 5) & 1u; // MMAG
            const bool wrap = (clamp & 3u) == 0u && ((clamp >> 2) & 3u) == 0u;
            if (linear)
                return wrap ? Vulkan::StockSampler::LinearWrap : Vulkan::StockSampler::LinearClamp;
            return wrap ? Vulkan::StockSampler::NearestWrap : Vulkan::StockSampler::NearestClamp;
        }
    }

    ParallelGS::ScanoutResult compose(Vulkan::Device &device, const ParallelGS::ScanoutResult &scanout, uint32_t displayFbp)
    {
        const Mode m = mode();
        if (m == Mode::Off || !scanout.image)
            return scanout;
        auto frame = frameForDisplay(displayFbp);
        if (!frame && m == Mode::Overlay)
            return scanout;

        const unsigned width = scanout.image->get_width(), height = scanout.image->get_height();
        auto colorInfo = Vulkan::ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        colorInfo.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto depthInfo = Vulkan::ImageCreateInfo::render_target(width, height, VK_FORMAT_D32_SFLOAT);
        depthInfo.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto color = device.create_image(colorInfo);
        auto depth = device.create_image(depthInfo);
        if (!color || !depth)
            return scanout;

        // parallel-gs builds Granite without SPIR-V reflection, so the layouts are spelled out here.
        static Vulkan::Program *program = [&device]
        {
            Vulkan::ResourceLayout vertexLayout, fragmentLayout;
            vertexLayout.input_mask = 0x7u; // position, colour, ST
            vertexLayout.push_constant_size = sizeof(PushConstants);
            fragmentLayout.output_mask = 0x1u;
            fragmentLayout.push_constant_size = sizeof(PushConstants);
            fragmentLayout.sets[0].sampled_image_mask = 0x1u;
            fragmentLayout.sets[0].fp_mask = 0x1u;
            fragmentLayout.sets[0].meta[0].array_size = 1;
            return device.request_program(native3d_vert, sizeof(native3d_vert), native3d_frag, sizeof(native3d_frag),
                                          &vertexLayout, &fragmentLayout);
        }();

        auto cmd = device.request_command_buffer();
        if (m == Mode::Overlay)
        {
            cmd->image_barrier(*color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            cmd->copy_image(*color, *scanout.image);
            cmd->image_barrier(*color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        else
        {
            cmd->image_barrier(*color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        cmd->image_barrier(*depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        Vulkan::RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &color->get_view();
        rp.depth_stencil = &depth->get_view();
        rp.store_attachments = 1u;
        if (m == Mode::Overlay)
            rp.load_attachments = 1u;
        else
            rp.clear_attachments = 1u;
        rp.op_flags = Vulkan::RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT;
        rp.clear_depth_stencil.depth = 0.0f;
        cmd->begin_render_pass(rp);

        if (frame && program)
        {
            cmd->set_program(program);
            cmd->set_opaque_state();
            cmd->set_cull_mode(VK_CULL_MODE_NONE);
            cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos));
            cmd->set_vertex_attrib(1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, rgba));
            cmd->set_vertex_attrib(2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, st));

            for (const Batch &batch : frame->batches)
            {
                if (batch.vertices.empty() || batch.indices.empty())
                    continue;
                const unsigned ctx = unsigned(batch.prim >> 9) & 1u;
                const uint64_t xyoffset = batch.regs.xyoffset[ctx];
                const uint64_t test = batch.regs.test[ctx];
                const bool tme = (batch.prim >> 4) & 1u;
                const bool abe = (batch.prim >> 6) & 1u;

                // Depth: ZTST 0 never, 1 always, 2 GEQUAL, 3 GREATER; ZBUF.ZMSK disables writes.
                static const VkCompareOp depthOps[4] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS,
                                                        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
                const bool zte = (test >> 16) & 1u;
                const bool zmask = (batch.regs.zbuf[ctx] >> 32) & 1u;
                cmd->set_depth_test(zte, zte && !zmask);
                cmd->set_depth_compare(depthOps[(test >> 17) & 3u]);

                if (abe)
                    applyBlend(*cmd, batch.regs.alpha[ctx]);
                else
                    cmd->set_blend_enable(false);

                // FRAME.FBMSK: a fully masked byte disables that channel.
                const uint32_t fbmsk = uint32_t(batch.regs.frame[ctx] >> 32);
                uint32_t writeMask = 0;
                for (unsigned ch = 0; ch < 4; ++ch)
                    if (((fbmsk >> (ch * 8u)) & 0xffu) != 0xffu)
                        writeMask |= 1u << ch;
                cmd->set_color_write_mask(writeMask);

                // Normally decoded long ago; waits only if the texture thread is behind.
                const std::shared_ptr<const Texture> texture = (tme && batch.texture) ? batch.texture->wait() : nullptr;
                cmd->set_texture(0, 0, textureView(device, texture.get()),
                                 sampler(batch.regs.tex1[ctx], batch.regs.clamp[ctx]));

                PushConstants pc{};
                std::memcpy(pc.objectToClip, batch.objectToClip, sizeof(pc.objectToClip));
                pc.screen[0] = batch.screenScale[0];
                pc.screen[1] = batch.screenScale[1];
                pc.screen[2] = batch.screenOffset[0];
                pc.screen[3] = batch.screenOffset[1];
                pc.target[0] = float(xyoffset & 0xffffu);
                pc.target[1] = float((xyoffset >> 32) & 0xffffu);
                pc.target[2] = 1.0f / float(width);
                pc.target[3] = 1.0f / float(height);
                pc.misc[0] = batch.depthScale / depthRange(batch.regs.zbuf[ctx]);
                const uint32_t tint = debugTint(batch.debugTag);
                std::memcpy(&pc.misc[1], &tint, sizeof(tint));
                pc.misc[2] = m == Mode::Overlay ? 0.35f : 0.0f;
                // PS2X_NATIVE_DEBUGVIEW: magenta = texture missing, cyan = texture all black, yellow = vertices black.
                static const bool debugView = std::getenv("PS2X_NATIVE_DEBUGVIEW") != nullptr;
                if (debugView)
                {
                    uint32_t flag = 0;
                    if (tme && !texture)
                        flag = 0xffff00ffu;
                    else if (texture && texture->black)
                        flag = 0xffffff00u;
                    else if (std::all_of(batch.vertices.begin(), batch.vertices.end(), [](const Vertex &v) { return (v.rgba & 0x00ffffffu) == 0u; }))
                        flag = 0xff00ffffu;
                    if (flag)
                    {
                        std::memcpy(&pc.misc[1], &flag, sizeof(flag));
                        pc.misc[2] = 1.0f;
                    }
                }
                const uint64_t tex0 = batch.regs.tex0[ctx];
                uint32_t bits = 0;
                if (texture)
                    bits |= 1u;
                bits |= uint32_t((tex0 >> 34) & 1u) << 1;          // TCC
                bits |= uint32_t((tex0 >> 35) & 3u) << 2;          // TFX
                bits |= uint32_t(test & 1u) << 4;                  // ATE
                bits |= uint32_t((test >> 1) & 7u) << 5;           // ATST
                bits |= uint32_t((test >> 12) & 3u) << 8;          // AFAIL
                bits |= uint32_t(batch.regs.fba[ctx] & 1u) << 10;  // FBA
                pc.shade[0] = float((test >> 4) & 0xffu);          // AREF
                std::memcpy(&pc.shade[1], &bits, sizeof(bits));
                cmd->push_constants(&pc, 0, sizeof(pc));

                const size_t vbytes = batch.vertices.size() * sizeof(Vertex);
                std::memcpy(cmd->allocate_vertex_data(0, vbytes, sizeof(Vertex)), batch.vertices.data(), vbytes);
                const size_t ibytes = batch.indices.size() * sizeof(uint16_t);
                std::memcpy(cmd->allocate_index_data(ibytes, VK_INDEX_TYPE_UINT16), batch.indices.data(), ibytes);
                cmd->draw_indexed(uint32_t(batch.indices.size()));
            }
        }
        cmd->end_render_pass();

        cmd->image_barrier(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        device.submit(cmd);
        evictTextures();

        ParallelGS::ScanoutResult result = scanout;
        result.image = color;
        return result;
    }
}
