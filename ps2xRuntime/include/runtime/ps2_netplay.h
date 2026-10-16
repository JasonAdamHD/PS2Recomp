#pragma once

// Netplay: deterministic lockstep over UDP. Every player runs the whole game; only pad inputs travel. Inputs are
// latched once per emulated vsync tick and every pad read during that tick returns them, so identical input
// streams give identical games. A player's input for tick T is sampled at tick T - delay, which hides the round
// trip. One player hosts (star topology: the host relays inputs between clients).
//
//   PS2X_NETPLAY=host[:port]              host a session (default port 7777)
//   PS2X_NETPLAY=join:<address>[:port]    join one
//   PS2X_NETPLAY_PLAYERS=<2..4>           players in the host's session (multitap slots 1..N; default 2)
//   PS2X_NETPLAY_DELAY=<frames>           input delay in vsync ticks (host; default: chosen from the measured pings)
//   PS2X_NETPLAY_SIM_PING / _JITTER / _LOSS   simulate this machine's link to the host (ms, ms, percent; testing)
//   PS2X_INPUT_RECORD=<file> / PS2X_INPUT_REPLAY=<file>   frame-locked single-player input recording (testing)
//   PS2X_FRAMEHASH=<ticks>                log a hash of EE RAM every N ticks (determinism checks)

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>

class PS2Runtime;

namespace ps2netplay
{
    struct PadInput
    {
        uint16_t buttons = 0xFFFF; // active-low PS2 button bits
        uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
    };

    // Inputs are frame-locked (netplay, record or replay): the runtime must avoid wall-clock influence on the guest.
    bool deterministic();

    // EE thread, at every VBlank start: samples local input, exchanges inputs with the other players and latches
    // this tick's inputs. Blocks while a peer's input for the tick has not arrived. `rdram` is hashed for desync
    // checks (32 MB).
    void onVBlank(PS2Runtime &runtime, const uint8_t *rdram, uint64_t tick);

    // The latched input of a player (0-3) for the current tick; false when the session has no such player.
    bool portInput(int player, PadInput &out);

    // sceCdReadClock: in deterministic mode, the session start time plus emulated time.
    std::time_t clock(std::time_t hostNow);

    // Netplay plays on a temporary copy of the host's memory card (sent to every client at session start), so all
    // players boot with the same saves and nobody's real card is written. The runtime registers how to read and
    // redirect its memory card folder.
    void setMemoryCardAccess(std::filesystem::path (*get)(), void (*set)(const std::filesystem::path &));

    // The local controller (pad port 1 of this machine), registered by the pad stubs.
    void setLocalSampler(bool (*sampler)(PS2Runtime &runtime, PadInput &out));
}
