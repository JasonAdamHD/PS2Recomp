// PS2X_NATIVE_SAMPLE=1 (Windows): samples the host instruction pointer of the EE game thread about 1000 times a
// second and prints the hottest image offsets every 10 seconds. Resolve them against the linker map of
// ps2EntryRunner (link with /MAP) to see which native functions - runtime, VU1, generated code - cost the time.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
    HANDLE g_gameThread = nullptr;
#endif
    std::atomic<bool> g_samplerStop{false};
    std::thread g_sampler;

    bool samplerEnabled()
    {
        static const bool enabled = std::getenv("PS2X_NATIVE_SAMPLE") != nullptr;
        return enabled;
    }
}

void ps2NativeSamplerRegisterThread(const char *role)
{
#ifdef _WIN32
    // PS2X_NATIVE_SAMPLE=gfx samples the graphics worker; any other value samples the EE game thread.
    const char *wanted = std::getenv("PS2X_NATIVE_SAMPLE");
    const bool wantsGfx = wanted && std::strcmp(wanted, "gfx") == 0;
    if (!samplerEnabled() || g_gameThread || wantsGfx != (std::strcmp(role, "gfx") == 0))
        return;
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        g_gameThread = handle;
    g_sampler = std::thread([]()
                            {
        const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::unordered_map<uint64_t, uint32_t> counts;
        std::unordered_map<std::string, uint32_t> moduleCounts;
        std::unordered_map<std::string, uint32_t> moduleOffsets;
        std::unordered_map<uint64_t, uint32_t> callerCounts;
        uint32_t total = 0u;
        auto lastReport = std::chrono::steady_clock::now();
        while (!g_samplerStop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (SuspendThread(g_gameThread) == static_cast<DWORD>(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(g_gameThread, &context) != 0;
            // While still suspended, unwind samples outside the executable to the first frame inside it, so
            // time in CRT/ntdll/driver code is charged to the native caller that led there.
            uint64_t imageCaller = 0u;
            const auto inImage = [&](uint64_t address)
            { return address >= imageBase && address < imageBase + 0x40000000ull; };
            if (ok && !inImage(context.Rip))
            {
                CONTEXT walk = context;
                for (int frame = 0; frame < 24 && walk.Rip != 0u; ++frame)
                {
                    if (inImage(walk.Rip))
                    {
                        imageCaller = walk.Rip - imageBase;
                        break;
                    }
                    DWORD64 functionBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(walk.Rip, &functionBase, nullptr);
                    if (!function)
                    {
                        walk.Rip = *reinterpret_cast<const DWORD64 *>(walk.Rsp);
                        walk.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, functionBase, walk.Rip, function, &walk, &handlerData,
                                     &establisherFrame, nullptr);
                }
            }
            ResumeThread(g_gameThread);
            if (!ok)
                continue;
            if (imageCaller != 0u)
                ++callerCounts[imageCaller];
            const uint64_t rip = context.Rip;
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module) &&
                module && reinterpret_cast<uintptr_t>(module) != imageBase)
            {
                // Outside the executable: bucket by module (DLL) and offset so CRT and driver time are visible.
                char name[MAX_PATH] = "?";
                GetModuleFileNameA(module, name, MAX_PATH);
                const char *base = std::strrchr(name, '\\');
                const std::string moduleName = base ? base + 1 : name;
                ++moduleCounts[moduleName];
                ++moduleOffsets[moduleName + "+" + std::to_string(rip - reinterpret_cast<uintptr_t>(module))];
            }
            else
                ++counts[rip - imageBase];
            ++total;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= std::chrono::seconds(10))
            {
                std::vector<std::pair<uint32_t, uint64_t>> ranked;
                ranked.reserve(counts.size());
                for (const auto &[offset, count] : counts)
                    ranked.emplace_back(count, offset);
                const size_t shown = std::min<size_t>(400u, ranked.size());
                std::partial_sort(ranked.begin(), ranked.begin() + shown, ranked.end(), std::greater<>());
                std::fprintf(stderr, "[native-samples] total=%u", total);
                for (size_t i = 0; i < shown; ++i)
                    std::fprintf(stderr, " %llx:%u", static_cast<unsigned long long>(ranked[i].second), ranked[i].first);
                std::fprintf(stderr, "\n[native-modules]");
                for (const auto &[moduleName, count] : moduleCounts)
                    std::fprintf(stderr, " %s:%u", moduleName.c_str(), count);
                std::vector<std::pair<uint32_t, std::string>> hotOffsets;
                for (const auto &[where, count] : moduleOffsets)
                    hotOffsets.emplace_back(count, where);
                std::sort(hotOffsets.rbegin(), hotOffsets.rend());
                std::fprintf(stderr, "\n[native-module-offsets]");
                for (size_t i = 0; i < hotOffsets.size() && i < 12u; ++i)
                    std::fprintf(stderr, " %s:%u", hotOffsets[i].second.c_str(), hotOffsets[i].first);
                std::fprintf(stderr, "\n");
                std::vector<std::pair<uint32_t, uint64_t>> callers;
                for (const auto &[offset, count] : callerCounts)
                    callers.emplace_back(count, offset);
                std::sort(callers.rbegin(), callers.rend());
                std::fprintf(stderr, "[native-ext-callers]");
                for (size_t i = 0; i < callers.size() && i < 40u; ++i)
                    std::fprintf(stderr, " %llx:%u", static_cast<unsigned long long>(callers[i].second), callers[i].first);
                std::fprintf(stderr, "\n");
                callerCounts.clear();
                moduleCounts.clear();
                moduleOffsets.clear();
                counts.clear();
                total = 0u;
                lastReport = now;
            }
        } });
#endif
}

void ps2NativeSamplerRegisterGameThread()
{
    ps2NativeSamplerRegisterThread("game");
}

void ps2NativeSamplerStop()
{
    g_samplerStop.store(true);
    if (g_sampler.joinable())
        g_sampler.join();
}
