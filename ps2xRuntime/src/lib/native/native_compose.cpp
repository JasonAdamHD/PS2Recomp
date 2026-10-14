#include "native_compose.h"
#include "runtime/native/native_renderer.h"

#include "device.hpp"
#include "command_buffer.hpp"

#include "native3d_vert.h"
#include "native3d_frag.h"

#include <cstddef>
#include <cstring>

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
        };
        static_assert(sizeof(PushConstants) == 112, "push constants must match native3d.vert");

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
            cmd->set_depth_test(true, true);
            cmd->set_depth_compare(VK_COMPARE_OP_GREATER_OR_EQUAL);
            cmd->set_cull_mode(VK_CULL_MODE_NONE);
            cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            if (m == Mode::Overlay)
            {
                cmd->set_blend_enable(true);
                cmd->set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
            }
            cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos));
            cmd->set_vertex_attrib(1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, rgba));
            cmd->set_vertex_attrib(2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, st));

            for (const Batch &batch : frame->batches)
            {
                if (batch.vertices.empty() || batch.indices.empty())
                    continue;
                const unsigned ctx = unsigned(batch.prim >> 9) & 1u;
                const uint64_t xyoffset = batch.regs.xyoffset[ctx];

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
                pc.misc[2] = m == Mode::Overlay ? 1.0f : 0.0f;
                pc.misc[3] = m == Mode::Overlay ? 0.6f : 1.0f;
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

        ParallelGS::ScanoutResult result = scanout;
        result.image = color;
        return result;
    }
}
