#include "runtime/gs/gs_threaded_backend.h"

#include "ThreadNaming.h"

#include <utility>

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner)),
      m_worker([this]()
               { workerLoop(); })
{
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopping = true;
    }
    m_workAvailable.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void GSThreadedBackend::enqueue(Command command)
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_pending.push_back(std::move(command));
        ++m_enqueued;
    }
    m_workAvailable.notify_one();
}

void GSThreadedBackend::drain() const
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueDrained.wait(lock, [this]()
                        { return m_completed == m_enqueued || m_stopping; });
}

void GSThreadedBackend::workerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSRaster");
    std::vector<Command> batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_workAvailable.wait(lock, [this]()
                                 { return !m_pending.empty() || m_stopping; });
            if (m_pending.empty() && m_stopping)
                return;
            batch.swap(m_pending);
        }

        for (Command &command : batch)
            execute(command);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_completed += batch.size();
        }
        batch.clear();
        m_queueDrained.notify_all();
    }
}

void GSThreadedBackend::execute(Command &command)
{
    std::visit([this](auto &item)
               {
                   using T = std::decay_t<decltype(item)>;
                   if constexpr (std::is_same_v<T, GSPrimitiveBatch>)
                       m_inner->Submit(item);
                   else if constexpr (std::is_same_v<T, ClutCommand>)
                       m_inner->LoadClut(item.tex0, item.texclut);
                   else if constexpr (std::is_same_v<T, GSTransferCommand>)
                       m_inner->BeginTransfer(item);
                   else if constexpr (std::is_same_v<T, UploadCommand>)
                       m_inner->UploadImage(item.data.data(), static_cast<uint32_t>(item.data.size()));
                   else if constexpr (std::is_same_v<T, WriteVramCommand>)
                       m_inner->WriteVram(item.psm, item.base, item.bw, item.x, item.y, item.value);
                   else if constexpr (std::is_same_v<T, FlushCommand>)
                       m_inner->Flush();
                   else if constexpr (std::is_same_v<T, TextureFlushCommand>)
                       m_inner->TextureFlush(); },
               command);
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    drain();
    m_inner->Initialize(vram, vramSize);
}

void GSThreadedBackend::Reset()
{
    drain();
    m_inner->Reset();
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    enqueue(batch);
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    enqueue(ClutCommand{tex0, texclut});
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    enqueue(command);
    // Local-to-host transfers are read back by the EE straight away; let them complete first.
    if (command.direction == 1u)
        drain();
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    UploadCommand upload;
    if (data && sizeBytes != 0u)
        upload.data.assign(data, data + sizeBytes);
    enqueue(std::move(upload));
}

void GSThreadedBackend::Flush()
{
    enqueue(FlushCommand{});
}

void GSThreadedBackend::TextureFlush()
{
    enqueue(TextureFlushCommand{});
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    // Presentation only needs a recent image; everything else (resets, FINISH, readbacks) needs all queued work.
    if (reason != GSSyncReason::Presentation)
        drain();
    m_inner->Sync(reason);
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    // The inner backend snapshots VRAM under its own lock, so it can run beside the worker. Not draining keeps
    // the host window from stalling behind a long GS command backlog.
    return m_inner->Present(request);
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    drain();
    return m_inner->ClearFramebuffer(context, rgba);
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    drain();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    drain();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    enqueue(WriteVramCommand{psm, base, bw, x, y, value});
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    drain();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    drain();
    return m_inner->GetTransferSnapshot();
}
