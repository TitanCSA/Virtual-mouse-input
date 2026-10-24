#pragma once
#include "Vdevice.h"
#include <string_view>

// A raw, seven-byte HID mouse report. X/Y are relative HID units, not screen
// coordinates; Windows pointer settings determine the resulting cursor motion.
namespace VirtualMouse {
constexpr std::uint8_t Endpoint = 0x81;
struct State {
    int x = 0, y = 0, wheel = 0, pan = 0;
    std::uint8_t buttons = 0; // Bits 0..4: left, right, middle, back, forward.
};
Vdevice::Device_Config Configuration();
Vdevice::Result Validate(const State& state);
// Throws std::invalid_argument for values outside this descriptor's limits.
std::vector<std::uint8_t> Report(const State& state);
// Decimal signed 16-bit motion; leading/trailing whitespace is accepted.
// On failure, value is left unchanged.
bool ParseMotion(std::wstring_view text, int& value);
}
