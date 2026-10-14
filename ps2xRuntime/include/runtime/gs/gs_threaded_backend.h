#pragma once

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

// Runs another raster backend on a worker thread. Commands that only change GS memory (primitives, CLUT
// loads, transfers, uploads, VRAM writes) are queued in order and return immediately, so the EE thread no
// longer waits for rasterisation. Anything that reads results back first waits for the queue to drain.
class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct ClutCommand
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };
    struct UploadCommand
    {
        std::vector<uint8_t> data;
    };
    struct WriteVramCommand
    {
        uint32_t psm, base, bw, x, y, value;
    };
    struct FlushCommand
    {
    };
    struct TextureFlushCommand
    {
    };
    using Command = std::variant<GSPrimitiveBatch, ClutCommand, GSTransferCommand, UploadCommand, WriteVramCommand,
                                 FlushCommand, TextureFlushCommand>;

    void enqueue(Command command);
    void drain() const;
    void workerLoop();
    void execute(Command &command);

    std::unique_ptr<GSRasterBackend> m_inner;
    mutable std::mutex m_queueMutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_queueDrained;
    std::vector<Command> m_pending;
    uint64_t m_enqueued = 0u;
    uint64_t m_completed = 0u;
    bool m_stopping = false;
    std::thread m_worker;
};
