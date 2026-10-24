#pragma once
#include "Mouse.h"
#include <windows.h>
#include <memory>
#include <string>

namespace VirtualMouse {
constexpr UINT SessionChanged = WM_APP + 1;
struct Snapshot {
    bool ready = false, starting = true;
    std::uint32_t error = 0;
    std::wstring title = L"Starting virtual mouse", detail;
    std::uint64_t reports = 0; // Reports accepted by Vdevice, not a delivery timestamp.
    State last{};
    std::uint64_t trial_remaining_100ns = 0;
    bool trial_expired = false, click_pending = false;
};
// Only the worker calls Vdevice. Windows messages and buttons never wait for I/O.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void Start(HWND window);
    bool Send(State state);
    bool Click(std::uint8_t button);
    void Stop();
    Snapshot Read();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
