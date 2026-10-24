#include "Mouse.h"
#include <algorithm>
#include <climits>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Field {
    unsigned page, usage, offset, bits, flags;
    int minimum, maximum;
};
struct Layout {
    std::vector<Field> fields;
    unsigned bits = 0, outputBits = 0, featureBits = 0;
    unsigned topPage = 0, topUsage = 0, collections = 0;
};

// Independent HID short-item interpreter for offline descriptor checks. This is
// test code only: Windows parses the descriptor when the real device is created.
Layout Parse(const std::vector<std::uint8_t>& descriptor) {
    Layout layout;
    unsigned page = 0, size = 0, count = 0, usageMinimum = 0, usageMaximum = 0;
    int minimum = 0, maximum = 0, depth = 0;
    std::vector<unsigned> usages;
    for (std::size_t position = 0; position < descriptor.size();) {
        const auto prefix = descriptor[position++];
        Check(prefix != 0xFE, "Unexpected HID long item");
        const unsigned length = (prefix & 3) == 3 ? 4 : prefix & 3;
        Check(position + length <= descriptor.size(), "Truncated HID item");
        unsigned value = 0;
        for (unsigned i = 0; i < length; ++i)
            value |= unsigned(descriptor[position++]) << (i * 8);
        const auto signedValue = [&] {
            if (!length) return 0;
            if (length == 4) return static_cast<int>(value);
            const unsigned sign = 1u << (length * 8 - 1);
            return int((value ^ sign)) - int(sign);
        };
        const unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: page = value; break;
            case 1: minimum = signedValue(); break;
            case 2: maximum = minimum < 0 ? signedValue() : int(value); break;
            case 7: size = value; break;
            case 8: Check(false, "Mouse report unexpectedly gained an ID"); break;
            case 9: count = value; break;
            default: Check(false, "Unexpected HID global item");
            }
        } else if (type == 2) {
            if (tag == 0) usages.push_back(value);
            else if (tag == 1) usageMinimum = value;
            else if (tag == 2) usageMaximum = value;
            else Check(false, "Unexpected HID local item");
        } else if (type == 0) {
            if (tag == 10) {
                if (!depth) {
                    layout.topPage = page;
                    layout.topUsage = usages.empty() ? 0 : usages.front();
                }
                ++depth;
                ++layout.collections;
            } else if (tag == 12) {
                Check(depth > 0, "Unbalanced HID collections");
                --depth;
            } else if (tag == 8 || tag == 9 || tag == 11) {
                Check(depth > 0 && size > 0 && size <= 32 && count <= 32,
                      "Invalid HID report field");
                if (tag == 8) {
                    for (unsigned i = 0; i < count; ++i) {
                        if (!(value & 1)) {
                            unsigned usage = usages.empty() ? 0 : usages[std::min<std::size_t>(i, usages.size() - 1)];
                            if (usageMinimum && usageMinimum + i <= usageMaximum)
                                usage = usageMinimum + i;
                            layout.fields.push_back({page, usage, layout.bits + i * size, size, value, minimum, maximum});
                        }
                    }
                    layout.bits += size * count;
                } else if (tag == 9) layout.outputBits += size * count;
                else layout.featureBits += size * count;
            } else Check(false, "Unexpected HID main item");
            usages.clear();
            usageMinimum = usageMaximum = 0;
        } else Check(false, "Reserved HID item");
    }
    Check(depth == 0, "Unclosed HID collection");
    return layout;
}
int Decode(const std::vector<std::uint8_t>& report, const Field& field) {
    unsigned value = 0;
    for (unsigned i = 0; i < field.bits; ++i) {
        const unsigned bit = field.offset + i;
        Check(bit / 8 < report.size(), "Report is too short");
        value |= unsigned((report[bit / 8] >> (bit % 8)) & 1) << i;
    }
    if (field.minimum >= 0) return static_cast<int>(value);
    const unsigned sign = 1u << (field.bits - 1);
    return int(value ^ sign) - int(sign);
}
void Reject(VirtualMouse::State state) {
    Check(!VirtualMouse::Validate(state), "Invalid state passed validation");
    bool rejected = false;
    try { (void)VirtualMouse::Report(state); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Invalid state was encoded");
}
}

int main() {
    try {
        using namespace VirtualMouse;
        const auto configuration = Configuration();
        const auto result = Vdevice::Validate_Device(configuration);
        if (!result) throw std::runtime_error(result.message);
        Check(configuration.speed == Vdevice::Speed::Full, "Mouse speed changed");
        Check(!configuration.delegate_controls, "Standard mouse delegates controls");
        Check(configuration.descriptors.size() == 7, "Missing USB descriptors");
        const auto& device = configuration.descriptors[0].data;
        Check(device.size() == 18 && device[8] == 0xFF && device[9] == 0xFF &&
              device[10] == 0x10 && device[11] == 0, "USB example identity changed");
        const auto& usb = configuration.descriptors[1].data;
        Check(usb.size() == 34 && usb[2] == usb.size(), "Configuration length mismatch");
        Check(usb[14] == 3 && usb[15] == 0 && usb[16] == 0, "Mouse incorrectly uses boot protocol");
        Check(usb[29] == Endpoint && usb[30] == 3 && usb[31] == 7 && usb[33] == 1,
              "Interrupt endpoint format changed");
        const auto& descriptor = configuration.descriptors[2];
        Check(descriptor.kind == Vdevice::Descriptor_Kind::Report && descriptor.interface_number == 0,
              "Report descriptor has the wrong interface");
        Check(usb[25] == (descriptor.data.size() & 255) && usb[26] == (descriptor.data.size() >> 8),
              "HID descriptor length mismatch");
        for (std::size_t i = 3; i < configuration.descriptors.size(); ++i) {
            const auto& text = configuration.descriptors[i].data;
            Check(text.size() >= 2 && text[0] == text.size() && text[1] == 3 && text.size() % 2 == 0,
                  "Malformed USB string descriptor");
        }
        const auto layout = Parse(descriptor.data);
        Check(layout.topPage == 1 && layout.topUsage == 2 && layout.collections == 2,
              "Report is not a mouse/pointer collection");
        Check(layout.bits == 56 && !layout.outputBits && !layout.featureBits && layout.fields.size() == 9,
              "Mouse report layout changed");
        for (unsigned i = 0; i < 5; ++i) {
            const auto& field = layout.fields[i];
            Check(field.page == 9 && field.usage == i + 1 && field.offset == i &&
                  field.bits == 1 && field.minimum == 0 && field.maximum == 1 && field.flags == 2,
                  "Mouse button descriptor changed");
        }
        for (unsigned i = 5; i < 9; ++i) {
            const auto& field = layout.fields[i];
            Check(field.flags == 6, "Mouse motion must be relative data");
            Check(field.page == (i == 8 ? 12u : 1u) &&
                  field.usage == (i == 5 ? 0x30u : i == 6 ? 0x31u : i == 7 ? 0x38u : 0x238u),
                  "Mouse axis/wheel usage changed");
            Check(field.bits == (i < 7 ? 16u : 8u) && field.minimum == (i < 7 ? -32768 : -127) &&
                  field.maximum == (i < 7 ? 32767 : 127), "Mouse motion range changed");
        }
        Check(Report({}) == std::vector<std::uint8_t>(7, 0), "Neutral report is not zero");
        for (int motion = -32768; motion <= 32767; ++motion) {
            State state;
            state.x = motion;
            state.y = -1 - motion;
            const auto report = Report(state);
            Check(report.size() == 7 && Decode(report, layout.fields[5]) == state.x &&
                  Decode(report, layout.fields[6]) == state.y, "Signed motion encoding failed");
            int parsed = INT_MAX;
            Check(ParseMotion(std::to_wstring(motion), parsed) && parsed == motion, "Decimal parsing failed");
        }
        for (int wheel = -127; wheel <= 127; ++wheel) {
            State state;
            state.wheel = wheel;
            state.pan = -wheel;
            const auto report = Report(state);
            Check(Decode(report, layout.fields[7]) == wheel && Decode(report, layout.fields[8]) == -wheel,
                  "Wheel/pan encoding failed");
        }
        for (unsigned buttons = 0; buttons < 32; ++buttons) {
            State state;
            state.buttons = static_cast<std::uint8_t>(buttons);
            const auto report = Report(state);
            for (unsigned i = 0; i < 5; ++i)
                Check(Decode(report, layout.fields[i]) == int((buttons >> i) & 1), "Button mapping failed");
            Check((report[0] & 0xE0) == 0, "Padding bits are not zero");
        }
        State state;
        for (const int bad : {INT_MIN, -32769, 32768, INT_MAX}) {
            state = {}; state.x = bad; Reject(state);
            state = {}; state.y = bad; Reject(state);
        }
        for (const int bad : {INT_MIN, -128, 128, INT_MAX}) {
            state = {}; state.wheel = bad; Reject(state);
            state = {}; state.pan = bad; Reject(state);
        }
        for (unsigned bad = 32; bad <= 255; ++bad) {
            state = {}; state.buttons = static_cast<std::uint8_t>(bad); Reject(state);
        }
        for (const auto text : {L"", L" ", L"+", L"-", L"32768", L"-32769", L"1.0", L"0x20", L"1x",
                               L"1 2", L"1\n2", L"--1", L"+-1", L"2147483648", L"-2147483649", L"１２"}) {
            int value = 123;
            Check(!ParseMotion(text, value) && value == 123, "Invalid text changed the motion value");
        }
        for (const auto text : {L"0", L"-0", L"+0", L" \t\n+000\r\v\f "}) {
            int value = 123;
            Check(ParseMotion(text, value) && value == 0, "Zero/whitespace parsing failed");
        }
        int value = 123;
        Check(!ParseMotion(std::wstring(100000, L'9'), value) && value == 123, "Long numeric overflow accepted");
        Check(!ParseMotion(std::wstring_view(L"1\0x", 3), value) && value == 123, "Embedded NUL accepted");
        auto malformed = configuration;
        malformed.descriptors[1].data[2] = 33;
        Check(!Vdevice::Validate_Device(malformed), "Wrong USB configuration length was accepted");
        malformed = configuration;
        malformed.descriptors[1].data[29] = 0;
        Check(!Vdevice::Validate_Device(malformed), "An invalid endpoint address was accepted");
        std::cout << checks << " offline checks passed. No driver was opened or installed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mouse test failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
