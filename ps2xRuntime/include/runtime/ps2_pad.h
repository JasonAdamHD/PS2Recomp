#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
};

// True when a (emulated) multitap is plugged into the given pad port (0 = port 1): players 1-4 then use its
// slots 0-3. Game code answers sceMtapGetConnection with this.
bool ps2PadMultitapConnected(int port);

#endif
