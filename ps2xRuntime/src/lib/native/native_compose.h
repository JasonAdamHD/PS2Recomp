#pragma once

#include "gs_renderer.hpp"

namespace ps2native
{
    // Draws the native frame for the displayed framebuffer page over (or instead of) the parallel-gs scanout.
    // Returns the scanout unchanged when the native renderer is off or has nothing for that page.
    ParallelGS::ScanoutResult compose(Vulkan::Device &device, const ParallelGS::ScanoutResult &scanout, uint32_t displayFbp);
}
