#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

namespace
{
    // Buttons seen held on any host frame since the guest last read the pad (active-high PAD_* bits).
    // Games here poll the pad only a few times per second, so a quick tap would otherwise fall between
    // two reads and be lost.
    std::atomic<uint16_t> g_latchedPresses{0u};

    int firstGamepad()
    {
        for (int i = 0; i < 4; ++i)
            if (IsGamepadAvailable(i))
                return i;
        return -1;
    }

    // Active-high mask of everything currently held on the keyboard and the first connected gamepad.
    uint16_t heldButtons()
    {
        uint16_t held = 0u;
        auto press = [&held](bool down, uint16_t mask)
        {
            if (down)
                held = static_cast<uint16_t>(held | mask);
        };

        const int pad = firstGamepad();
        if (pad >= 0)
        {
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_UP), PAD_UP);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN), PAD_DOWN);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT), PAD_LEFT);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT), PAD_RIGHT);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN), PAD_CROSS);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT), PAD_CIRCLE);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT), PAD_SQUARE);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_UP), PAD_TRIANGLE);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_1), PAD_L1);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1), PAD_R1);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_2), PAD_L2);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2), PAD_R2);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_MIDDLE_RIGHT), PAD_START);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_MIDDLE_LEFT), PAD_SELECT);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_THUMB), PAD_L3);
            press(IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_THUMB), PAD_R3);
        }

        press(IsKeyDown(KEY_UP) || IsKeyDown(KEY_W), PAD_UP);
        press(IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S), PAD_DOWN);
        press(IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A), PAD_LEFT);
        press(IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D), PAD_RIGHT);
        press(IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE), PAD_CROSS);
        press(IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE), PAD_CIRCLE);
        press(IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0), PAD_SQUARE);
        press(IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1), PAD_TRIANGLE);
        press(IsKeyDown(KEY_Q), PAD_L1);
        press(IsKeyDown(KEY_E), PAD_R1);
        press(IsKeyDown(KEY_LEFT_SHIFT), PAD_L2);
        press(IsKeyDown(KEY_RIGHT_SHIFT), PAD_R2);
        press(IsKeyDown(KEY_ENTER), PAD_START);
        press(IsKeyDown(KEY_TAB), PAD_SELECT);
        return held;
    }

    // Pseudo-buttons for the scripted left stick (not real PS2 pad bits; never reported as buttons).
    constexpr uint32_t STICK_LEFT = 0x10000u;
    constexpr uint32_t STICK_RIGHT = 0x20000u;
    constexpr uint32_t STICK_UP = 0x40000u;
    constexpr uint32_t STICK_DOWN = 0x80000u;

    // Test aid: PS2X_AUTOPRESS="cross@600,start@640" holds the named button for ~1.5 s at each listed
    // number of seconds after launch, so automated runs can step through menus. "lleft", "lright", "lup" and
    // "ldown" push the left stick fully that way instead.
    uint32_t scriptedInputs()
    {
        struct ScriptedPress
        {
            double atSeconds = 0.0;
            uint32_t bit = 0;
        };
        static const std::vector<ScriptedPress> presses = []
        {
            std::vector<ScriptedPress> parsed;
            const char *script = std::getenv("PS2X_AUTOPRESS");
            if (!script)
                return parsed;
            const std::pair<const char *, uint32_t> names[] = {
                {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE},
                {"start", PAD_START}, {"select", PAD_SELECT}, {"up", PAD_UP}, {"down", PAD_DOWN},
                {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"lleft", STICK_LEFT}, {"lright", STICK_RIGHT},
                {"lup", STICK_UP}, {"ldown", STICK_DOWN}};
            std::string text(script);
            size_t begin = 0;
            while (begin < text.size())
            {
                const size_t end = std::min(text.find(',', begin), text.size());
                const std::string item = text.substr(begin, end - begin);
                const size_t at = item.find('@');
                if (at != std::string::npos)
                {
                    const std::string name = item.substr(0, at);
                    for (const auto &[label, bit] : names)
                        if (name == label)
                            parsed.push_back({std::atof(item.c_str() + at + 1), bit});
                }
                begin = end + 1;
            }
            return parsed;
        }();
        static const auto start = std::chrono::steady_clock::now();
        uint32_t held = 0u;
        if (!presses.empty())
        {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            for (const ScriptedPress &press : presses)
                if (now >= press.atSeconds && now < press.atSeconds + 1.5)
                    held |= press.bit;
        }
        return held;
    }
}

// Called by the host render loop once per displayed frame to latch presses between guest pad reads.
void ps2PadSampleHostInput()
{
    g_latchedPresses.fetch_or(heldButtons(), std::memory_order_relaxed);
}

bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    // One controller, in port 1: other ports and multitap slots read as an idle pad.
    if (port != 0 || slot != 0)
        return true;

    const uint32_t scripted = scriptedInputs();
    const uint16_t held = static_cast<uint16_t>(heldButtons() | (scripted & 0xFFFFu) |
                                                g_latchedPresses.exchange(0u, std::memory_order_relaxed));
    const uint16_t btns = static_cast<uint16_t>(~held);

    const int pad = firstGamepad();
    if (pad >= 0)
    {
        const float lx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X);
        const float ly = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);
        const float rx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X);
        const float ry = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    // The scripted stick wins over a connected gamepad so automated runs behave the same on any machine.
    if (scripted & STICK_LEFT)
        data[6] = 0x00;
    if (scripted & STICK_RIGHT)
        data[6] = 0xFF;
    if (scripted & STICK_UP)
        data[7] = 0x00;
    if (scripted & STICK_DOWN)
        data[7] = 0xFF;

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
