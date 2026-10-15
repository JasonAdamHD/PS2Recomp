#pragma once

// Native renderer: draws VU1-transformed 3D directly on the host GPU instead of through VU1 + GS emulation.
// Game code (overrides) decodes VU1 batches into ps2native::Batch and submits them from the VIF1 thread; the
// GS backend composites the collected frame at presentation time. See docs/native-renderer.md in the game repo.

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace ps2native
{
    // PS2X_NATIVE3D: 0/unset = off, 1 = overlay native 3D on the emulated frame, 2 = native 3D only,
    // 3 = native 3D only and handled VU1 batches skip the interpreter (no emulated 3D at all),
    // 4 = full native frame: as 3, plus every other GS primitive drawn into the frame's buffer is replayed
    //     natively in stream order (2D, effects, and the output of VU1 programs without a native port).
    enum class Mode : uint8_t
    {
        Off = 0,
        Overlay = 1,
        Only = 2,
        Replace = 3,
        Full = 4,
    };
    Mode mode();
    inline bool enabled() { return mode() != Mode::Off; }
    // Handled VU1 batches skip the interpreter.
    inline bool replacesVu1() { return mode() == Mode::Replace || mode() == Mode::Full; }

    struct Vertex
    {
        float pos[3];  // object space, before the batch matrix (screen-space batches: GS window x, y in 12.4, Z)
        uint32_t rgba; // vertex colour bytes as the GS sees them (0x80 = 1.0)
        float st[2];   // normalised texture coordinates
        float q = 1.0f; // screen-space batches: texture lookup at st / q (GS STQ)
    };

    // A GS texture decoded to RGBA8 (bytes as the GS sees them: alpha 0x80 = 1.0).
    struct Texture
    {
        uint64_t serial = 0; // unique per decode; the presenter caches GPU images by it
        uint32_t width = 0, height = 0;
        bool black = false; // every texel has RGB 0 (debug view)
        std::vector<uint32_t> rgba;
    };

    // A texture a batch samples, resolved asynchronously by the texture thread.
    class TextureRequest
    {
    public:
        // Blocks until the texture thread has decoded it; null when the format is unsupported.
        std::shared_ptr<const Texture> wait();
        void resolve(std::shared_ptr<const Texture> texture);

    private:
        std::mutex mutex_;
        std::condition_variable ready_;
        bool done_ = false;
        std::shared_ptr<const Texture> texture_;
    };

    // The texture thread keeps a CPU shadow of GS local memory. Both calls queue work for it in stream order
    // and return at once; call them before the GS consumes the packet / for the batch being decoded.
    void observeGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes);
    std::shared_ptr<TextureRequest> requestTexture(uint64_t tex0, uint64_t texa);
    // Mode::Full: replays the GS primitives of a GIF packet that draw into the native frame as screen-space
    // batches, in stream order with the VU1 batches. Call before the GS consumes the packet.
    void replayGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes);
    // The current GS drawing registers (the replay starts every packet from them).
    void setRegisterSource(bool (*source)(GSRawDrawRegisters &out));

    // Debug: waits for the texture thread to drain, then reads one pixel of the shadow (GS PSM code).
    uint32_t debugShadowRead(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);

    struct Batch
    {
        float objectToClip[16]{}; // column-major: clip = M * (pos, 1)
        float screenScale[2]{};   // GS window coords (12.4) = clip.xy / clip.w * screenScale + screenOffset
        float screenOffset[2]{};
        float depthScale = 0.0f; // GS Z = depthScale / clip.w
        uint64_t prim = 0;       // PRIM the GS would use for this batch
        GSRawDrawRegisters regs{};
        uint32_t debugTag = 0; // identifies the source microprogram (debug colouring)
        bool screenSpace = false; // vertices are already GS window coordinates (replayed GS primitives)
        std::shared_ptr<TextureRequest> texture; // null when PRIM.TME is off
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices; // triangle list
    };

    // Thread-safe; called on the VIF1 thread.
    void submit(Batch &&batch);
    // Reports a GS primitive drawn (not through the native path) into framebuffer page fbp. Games that render
    // into an off-screen page and copy it to the display page use this to mark the end of the frame.
    void noteGsDraw(uint32_t fbp);
    // Whether framebuffer page fbp is the one the native frame is drawn into (not a display page or a render
    // target). True for anything but display pages until the game's buffer setup has been observed.
    bool isDrawBuffer(uint32_t fbp);

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
