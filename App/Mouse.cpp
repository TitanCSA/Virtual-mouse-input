#include "Mouse.h"
#include <stdexcept>
#include <utility>

namespace VirtualMouse {
namespace {
Vdevice::Descriptor Text(std::uint32_t index, std::u16string_view text) {
    Vdevice::Descriptor descriptor;
    descriptor.index = index;
    descriptor.language = 0x0409;
    descriptor.data = {static_cast<std::uint8_t>(2 + text.size() * 2), 3};
    for (const auto character : text) {
        descriptor.data.push_back(static_cast<std::uint8_t>(character));
        descriptor.data.push_back(static_cast<std::uint8_t>(character >> 8));
    }
    return descriptor;
}
bool Space(wchar_t character) {
    return character == L' ' || character == L'\t' || character == L'\r' ||
           character == L'\n' || character == L'\v' || character == L'\f';
}
}

Vdevice::Device_Config Configuration() {
    Vdevice::Device_Config configuration;
    // Full-speed USB, interface-defined class, example VID FFFF / PID 0010.
    Vdevice::Descriptor device;
    device.data = {18,1,0,2,0,0,0,64,0xFF,0xFF,0x10,0,0,1,1,2,3,1};
    Vdevice::Descriptor report;
    report.kind = Vdevice::Descriptor_Kind::Report;
    report.data = {
        0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00, // Mouse / pointer.
        0x05,0x09,0x19,0x01,0x29,0x05,0x15,0,0x25,1,
        0x95,5,0x75,1,0x81,2, // Five buttons.
        0x95,1,0x75,3,0x81,1, // Three padding bits.
        0x05,1,0x09,0x30,0x09,0x31,0x16,0,0x80,0x26,0xFF,0x7F,
        0x75,0x10,0x95,2,0x81,6, // Relative, signed 16-bit X and Y.
        0x09,0x38,0x15,0x81,0x25,0x7F,0x75,8,0x95,1,0x81,6, // Wheel.
        0x05,0x0C,0x0A,0x38,2,0x95,1,0x81,6,0xC0,0xC0 // AC Pan.
    };
    Vdevice::Descriptor usb;
    usb.data = {
        9,2,34,0,1,1,0,0x80,50, // One bus-powered configuration, 100 mA.
        9,4,0,0,1,3,0,0,0, // HID, non-boot interface (16-bit motion).
        9,0x21,0x11,1,0,1,0x22,
        static_cast<std::uint8_t>(report.data.size()),
        static_cast<std::uint8_t>(report.data.size() >> 8),
        7,5,Endpoint,3,7,0,1 // Interrupt IN; seven bytes, 1 ms interval.
    };
    Vdevice::Descriptor languages;
    languages.data = {4,3,0x09,0x04};
    configuration.descriptors = {
        std::move(device), std::move(usb), std::move(report), std::move(languages),
        Text(1,u"Vdevice examples"), Text(2,u"Virtual mouse input"),
        Text(3,u"VirtualMouseInput-001")
    };
    return configuration;
}

Vdevice::Result Validate(const State& state) {
    if (state.x < -32768 || state.x > 32767 || state.y < -32768 || state.y > 32767)
        return {87, "X and Y must be between -32768 and 32767."};
    if (state.wheel < -127 || state.wheel > 127 || state.pan < -127 || state.pan > 127)
        return {87, "Wheel and pan must be between -127 and 127."};
    if (state.buttons & ~31u)
        return {87, "Only the five mouse button bits can be set."};
    return {};
}

std::vector<std::uint8_t> Report(const State& state) {
    const auto validation = Validate(state);
    if (!validation) throw std::invalid_argument(validation.message);
    const auto x = static_cast<std::uint16_t>(state.x);
    const auto y = static_cast<std::uint16_t>(state.y);
    return {state.buttons,
            static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8),
            static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8),
            static_cast<std::uint8_t>(state.wheel), static_cast<std::uint8_t>(state.pan)};
}

bool ParseMotion(std::wstring_view text, int& value) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    while (!text.empty() && Space(text.back())) text.remove_suffix(1);
    if (text.empty()) return false;
    const bool negative = text.front() == L'-';
    if (negative || text.front() == L'+') text.remove_prefix(1);
    if (text.empty()) return false;
    const unsigned limit = negative ? 32768 : 32767;
    unsigned number = 0;
    for (const auto character : text) {
        if (character < L'0' || character > L'9') return false;
        const unsigned digit = character - L'0';
        if (number > (limit - digit) / 10) return false;
        number = number * 10 + digit;
    }
    value = negative ? -static_cast<int>(number) : static_cast<int>(number);
    return true;
}
}
