#pragma once

// Native renderer: draws VU1-transformed 3D directly on the host GPU instead of through VU1 + GS emulation.
// Game code (overrides) decodes VU1 batches into ps2native::Batch and submits them from the VIF1 thread; the
// GS backend composites the collected frame at presentation time. See docs/native-renderer.md in the game repo.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ps2native
{
    // PS2X_NATIVE3D: 0/unset = off, 1 = overlay native 3D on the emulated frame, 2 = native 3D only.
    enum class Mode : uint8_t
    {
        Off = 0,
        Overlay = 1,
        Only = 2,
    };
    Mode mode();
    inline bool enabled() { return mode() != Mode::Off; }

    struct Vertex
    {
        float pos[3];  // object space, before the batch matrix
        uint32_t rgba; // vertex colour bytes as the GS sees them (0x80 = 1.0)
        float st[2];
    };

    struct Batch
    {
        float objectToClip[16]{}; // column-major: clip = M * (pos, 1)
        float screenScale[2]{};   // GS window coords (12.4) = clip.xy / clip.w * screenScale + screenOffset
        float screenOffset[2]{};
        float depthScale = 0.0f; // GS Z = depthScale / clip.w
        uint64_t prim = 0;       // PRIM the GS would use for this batch
        GSRawDrawRegisters regs{};
        uint32_t debugTag = 0; // identifies the source microprogram (debug colouring)
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices; // triangle list
    };

    // Thread-safe; called on the VIF1 thread.
    void submit(Batch &&batch);
    // Reports a GS primitive drawn (not through the native path) into framebuffer page fbp. Games that render
    // into an off-screen page and copy it to the display page use this to mark the end of the frame.
    void noteGsDraw(uint32_t fbp);

    struct Frame
    {
        uint32_t fbp = 0;
        std::vector<Batch> batches;
    };

    // Presentation side: the most recent complete frame drawn into framebuffer page fbp (nullptr if none).
    std::shared_ptr<const Frame> frameForDisplay(uint32_t displayFbp);

    struct Stats
    {
        uint64_t batches = 0, vertices = 0, triangles = 0, frames = 0;
    };
    Stats takeStats();
}
