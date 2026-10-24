#include "Session.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace VirtualMouse {
namespace {
using Clock = std::chrono::steady_clock;
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(size, L' ');
    if (size) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), size);
    return value;
}
}
struct Session::Impl {
    struct Command { State state{}; std::uint8_t click = 0; };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> queue;
    std::thread worker;
    HWND window = nullptr;
    Snapshot snapshot;
    bool stopping = false, notification = false;

    void Notify() { // Called with mutex held. PostMessage never calls application code.
        if (!notification && window) notification = PostMessageW(window, SessionChanged, 0, 0) != FALSE;
    }
    template<class Function> void Change(Function function) {
        std::lock_guard<std::mutex> guard(mutex);
        function(snapshot);
        Notify();
    }
    bool Stopped() {
        std::lock_guard<std::mutex> guard(mutex);
        return stopping;
    }
    void Fail(std::uint32_t error, const std::wstring& message) {
        std::lock_guard<std::mutex> guard(mutex);
        snapshot.ready = snapshot.starting = snapshot.click_pending = false;
        snapshot.error = error;
        snapshot.title = L"Virtual mouse unavailable";
        snapshot.detail = message + L" (Windows error " + std::to_wstring(error) + L").";
        queue.clear();
        Notify();
    }
    bool Refresh(const Vdevice::Device& device) {
        const auto info = Vdevice::Get_Info();
        Change([&](Snapshot& state) {
            state.trial_remaining_100ns = info.trial_remaining_100ns;
            state.trial_expired = info.trial_expired;
        });
        if (!info) { Fail(info.error, Wide(info.message)); return false; }
        if (info.trial_expired) { Fail(ERROR_ACCOUNT_RESTRICTION, L"The Vdevice demo has expired. Restart Windows to begin another session"); return false; }
        if (info.quiescing || info.removing) { Fail(ERROR_NOT_READY, L"Vdevice is stopping or waiting for a Windows restart"); return false; }
        const auto found = std::find_if(info.devices.begin(), info.devices.end(), [&](const auto& item) { return item.id == device.Id(); });
        if (found == info.devices.end() || !found->plugged) { Fail(ERROR_DEVICE_NOT_CONNECTED, L"The virtual mouse was disconnected"); return false; }
        if (found->error) { Fail(found->error, L"Windows reported a virtual mouse error"); return false; }
        const bool ready = found->configured && found->powered;
        Change([&](Snapshot& state) {
            state.ready = ready; state.starting = !ready;
            state.title = ready ? L"Virtual mouse connected" : L"Windows is preparing the mouse";
            state.detail = ready ? L"Ready. Inputs use Vdevice raw HID reports." : L"Waiting for Windows USB and HID enumeration.";
        });
        return true;
    }
    bool Output() {
        // Consume bounded enumeration notifications without waiting for input.
        for (unsigned count = 0; count < 16; ++count) {
            const auto output = Vdevice::Get_Output(0);
            if (!output) {
                if (output.timed_out || output.error == ERROR_TIMEOUT || output.error == ERROR_NO_MORE_ITEMS || output.error == ERROR_NO_DATA) return true;
                Fail(output.error, Wide(output.message)); return false;
            }
            if (output.kind == Vdevice::Output_Kind::Control) {
                const auto result = Vdevice::Respond_Output(output, {}, true);
                if (!result) { Fail(result.error, Wide(result.message)); return false; }
            }
        }
        return true;
    }
    bool Transmit(const Vdevice::Device& device, State state) {
        const auto result = Vdevice::Send_Input(device, Endpoint, Report(state));
        if (!result) { Fail(result.error, Wide(result.message)); return false; }
        Change([&](Snapshot& snapshot) {
            ++snapshot.reports; snapshot.last = state; snapshot.error = 0;
        });
        return true;
    }
    bool PointerOutside() const {
        POINT point{};
        if (!GetCursorPos(&point)) return false;
        const HWND hit = WindowFromPoint(point);
        return hit && GetAncestor(hit, GA_ROOTOWNER) != GetAncestor(window, GA_ROOTOWNER);
    }
    void Run() noexcept {
        Vdevice::Device device;
        bool healthy = false;
        try {
            const auto before = Vdevice::Get_Info();
            if (!before) { Fail(before.error, Wide(before.message)); return; }
            if (before.trial_expired || before.quiescing || before.removing) {
                Change([&](Snapshot& value) { value.trial_expired = before.trial_expired; });
                Fail(before.trial_expired ? ERROR_ACCOUNT_RESTRICTION : ERROR_NOT_READY,
                    before.trial_expired ? L"The Vdevice demo has expired. Restart Windows" : L"Vdevice is stopping. Restart Windows before creating a mouse");
                return;
            }
            device = Vdevice::Create_Device(Configuration());
            if (!device) { Fail(device.error, Wide(device.message)); return; }
            healthy = true;
            const auto began = Clock::now();
            auto nextPoll = began;
            auto clickAt = (Clock::time_point::max)();
            std::uint8_t click = 0;
            bool configuredOnce = false;
            while (healthy) {
                std::deque<Command> commands;
                {
                    std::unique_lock<std::mutex> guard(mutex);
                    wake.wait_until(guard, (std::min)(nextPoll, clickAt), [&] { return stopping || !queue.empty(); });
                    if (stopping) break;
                    commands.swap(queue);
                }
                const auto now = Clock::now();
                if (now >= nextPoll || !commands.empty() || (click && now >= clickAt)) {
                    healthy = Output() && Refresh(device);
                    if (!healthy) break;
                    bool ready;
                    { std::lock_guard<std::mutex> guard(mutex); ready = snapshot.ready; }
                    configuredOnce = configuredOnce || ready;
                    nextPoll = now + (ready ? std::chrono::milliseconds(1000) : std::chrono::milliseconds(50));
                    if (!ready) {
                        if (!configuredOnce && now - began > std::chrono::seconds(30)) { Fail(ERROR_TIMEOUT, L"Windows did not finish configuring the mouse within 30 seconds"); healthy = false; }
                        if (!commands.empty() || click) {
                            click = 0; clickAt = (Clock::time_point::max)();
                            Change([](Snapshot& value) { value.click_pending = false; value.error = ERROR_NOT_READY; value.detail = L"The mouse is not configured and powered. Wait until it is ready."; });
                        }
                        continue;
                    }
                }
                for (const auto& command : commands) {
                    if (Stopped()) break;
                    if (command.click) { click = command.click; clickAt = Clock::now() + std::chrono::milliseconds(1200); }
                    else if (!Transmit(device, command.state)) { healthy = false; break; }
                }
                if (Stopped()) break;
                if (healthy && click && Clock::now() >= clickAt) {
                    if (PointerOutside()) {
                        State pressed{}; pressed.buttons = click;
                        // Press and release are separate FIFO reports, without a sleep.
                        healthy = Transmit(device, pressed) && Transmit(device, {});
                    } else Change([](Snapshot& value) {
                        value.error = ERROR_CANCELLED;
                        value.detail = L"Click cancelled. Move the pointer outside this application before the click fires.";
                    });
                    click = 0; clickAt = (Clock::time_point::max)();
                    Change([](Snapshot& value) { value.click_pending = false; });
                }
            }
        } catch (const std::exception& error) {
            Fail(ERROR_UNHANDLED_EXCEPTION, Wide(error.what()));
        } catch (...) {
            Fail(ERROR_UNHANDLED_EXCEPTION, L"Unexpected mouse worker error");
        }
        if (device) try {
            // Best effort: release held buttons, then remove the owned device.
            Vdevice::Send_Input(device, Endpoint, Report({}));
            const auto removed = Vdevice::Delete_Device(device);
            if (!removed && healthy) Fail(removed.error, L"The virtual mouse could not be removed: " + Wide(removed.message));
        } catch (...) { /* The SDK device owner is the final cleanup fallback. */ }
        Change([](Snapshot& value) { value.ready = value.starting = value.click_pending = false; });
    }
};
Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() { Stop(); }
void Session::Start(HWND window) {
    Stop();
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->window = window; impl_->snapshot = {}; impl_->stopping = false; impl_->notification = false;
    impl_->queue.clear();
    try { impl_->worker = std::thread([this] { impl_->Run(); }); }
    catch (...) { impl_->snapshot.starting = false; impl_->snapshot.error = ERROR_NOT_ENOUGH_MEMORY; impl_->snapshot.title = L"Could not start mouse worker"; }
    impl_->Notify();
}
bool Session::Send(State state) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->snapshot.ready || impl_->stopping) return false;
    const auto valid = Validate(state);
    if (!valid || impl_->queue.size() >= 64) {
        impl_->snapshot.error = valid ? ERROR_BUSY : valid.error;
        impl_->snapshot.detail = valid ? L"The input queue is full. Wait before sending more reports." : Wide(valid.message);
        impl_->Notify(); return false;
    }
    impl_->queue.push_back({state, 0}); impl_->wake.notify_one(); return true;
}
bool Session::Click(std::uint8_t button) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->snapshot.ready || impl_->stopping || impl_->snapshot.click_pending || !button || (button & ~31u) || impl_->queue.size() >= 64) return false;
    impl_->queue.push_back({{}, button});
    impl_->snapshot.click_pending = true; impl_->snapshot.error = 0;
    impl_->snapshot.detail = L"Click armed. Move the pointer outside this application within 1.2 seconds.";
    impl_->Notify(); impl_->wake.notify_one(); return true;
}
void Session::Stop() {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->stopping = true; impl_->queue.clear(); impl_->wake.notify_one();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
}
Snapshot Session::Read() {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->notification = false; return impl_->snapshot;
}
}
