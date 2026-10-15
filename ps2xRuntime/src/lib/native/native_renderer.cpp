#include "runtime/native/native_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace ps2native
{
    namespace
    {
        // Batches are grouped by the framebuffer page they draw into. A frame for display buffer A is complete
        // when drawing moves on to the other display buffer; render-to-texture targets in between do not end it.
        struct State
        {
            std::mutex mutex;
            std::map<uint32_t, std::vector<Batch>> open;
            std::map<uint32_t, std::shared_ptr<const Frame>> complete;
            uint32_t displayFbps[4]{};
            unsigned displayCount = 0;
            uint32_t lastFbp = ~0u;
            Stats stats;

            bool isDisplay(uint32_t fbp) const
            {
                return std::find(displayFbps, displayFbps + displayCount, fbp) != displayFbps + displayCount;
            }

            // Ends the frame drawn into fbp; it is shown from displayFbp (itself, or the page it is copied to).
            void close(uint32_t fbp, uint32_t displayFbp)
            {
                auto it = open.find(fbp);
                if (it == open.end() || it->second.empty())
                    return;
                auto frame = std::make_shared<Frame>();
                frame->fbp = fbp;
                frame->batches = std::move(it->second);
                it->second.clear();
                complete[displayFbp] = std::move(frame);
                ++stats.frames;
            }
        };

        State &state()
        {
            static State s;
            return s;
        }

        // Pages that never reach the display (render targets) are dropped once they hold this many batches.
        constexpr size_t kMaxOpenBatches = 65536;
    }

    Mode mode()
    {
        static const Mode value = []
        {
            const char *env = std::getenv("PS2X_NATIVE3D");
            if (!env)
                return Mode::Off;
            switch (std::atoi(env))
            {
            case 1:
                return Mode::Overlay;
            case 2:
                return Mode::Only;
            case 3:
                return Mode::Replace;
            default:
                return Mode::Off;
            }
        }();
        return value;
    }

    void submit(Batch &&batch)
    {
        auto &s = state();
        const uint32_t fbp = uint32_t(batch.regs.frame[(batch.prim >> 9) & 1] & 0x1ffu);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.lastFbp != ~0u && fbp != s.lastFbp)
        {
            // Until presentation has reported the display buffers, any switch ends the frame.
            const bool knownDisplays = s.displayCount >= 2;
            if (!knownDisplays || (s.isDisplay(fbp) && s.isDisplay(s.lastFbp)))
                s.close(s.lastFbp, s.lastFbp);
            else if (s.isDisplay(fbp))
            {
                // Coming back from a render target to a display buffer: the other display buffer is done.
                for (unsigned i = 0; i < s.displayCount; ++i)
                    if (s.displayFbps[i] != fbp)
                        s.close(s.displayFbps[i], s.displayFbps[i]);
            }
        }
        s.lastFbp = fbp;
        auto &list = s.open[fbp];
        if (list.size() >= kMaxOpenBatches)
            list.clear();
        ++s.stats.batches;
        s.stats.vertices += batch.vertices.size();
        s.stats.triangles += batch.indices.size() / 3u;
        list.push_back(std::move(batch));
    }

    void noteGsDraw(uint32_t fbp)
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        // A draw into a display page from another page's frame is the copy that presents it.
        if (s.lastFbp != ~0u && fbp != s.lastFbp && s.isDisplay(fbp) && !s.isDisplay(s.lastFbp))
            s.close(s.lastFbp, fbp);
    }

    std::shared_ptr<const Frame> frameForDisplay(uint32_t displayFbp)
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.isDisplay(displayFbp))
        {
            std::fprintf(stderr, "[native] display buffer page 0x%x\n", displayFbp);
            if (s.displayCount < 4)
                s.displayFbps[s.displayCount++] = displayFbp;
            else
            {
                std::rotate(s.displayFbps, s.displayFbps + 1, s.displayFbps + 4);
                s.displayFbps[3] = displayFbp;
            }
        }
        auto it = s.complete.find(displayFbp);
        return it == s.complete.end() ? nullptr : it->second;
    }

    Stats takeStats()
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        Stats out = s.stats;
        s.stats = {};
        return out;
    }
}
