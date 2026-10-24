// Offline transport double: no driver handles, virtual devices or real inputs.
#include "Session.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Fake {
std::atomic<unsigned> infoError{}, sendError{}, creates{}, deletes{};
std::atomic<unsigned> sendCalls{}, pauseAt{};
std::atomic<bool> created{}, configured{true}, expired{}, quiescing{}, throwInfo{}, block{}, sending{};
std::mutex mutex;
std::condition_variable wake;
std::vector<std::vector<std::uint8_t>> reports;
void Reset() {
    infoError = sendError = creates = deletes = 0;
    sendCalls = pauseAt = 0;
    created = expired = quiescing = throwInfo = block = sending = false;
    configured = true;
    std::lock_guard<std::mutex> lock(mutex); reports.clear();
}
}
namespace Vdevice {
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
Device::operator bool() const noexcept { return !token.empty() && !error; }
std::uint64_t Device::Id() const noexcept { return token.empty() ? 0 : 1; }
Info Get_Info() {
    if (Fake::throwInfo) throw std::runtime_error("Offline transport failure");
    Info value;
    value.error = Fake::infoError;
    if (value.error) value.message = "Offline driver is unavailable";
    value.trial_expired = Fake::expired;
    value.quiescing = Fake::quiescing;
    value.trial_remaining_100ns = 36000000000ull;
    if (Fake::created) {
        Device_Details details;
        details.id = 1; details.plugged = true;
        details.powered = details.configured = Fake::configured;
        value.devices.push_back(details);
    }
    return value;
}
Device Create_Device(const Device_Config&) {
    ++Fake::creates; Fake::created = true;
    Device value; value.token = "offline-mouse"; return value;
}
Result Delete_Device(Device& value) {
    ++Fake::deletes; Fake::created = false; value.token.clear(); return {};
}
Result Send_Input(const Device&, std::uint8_t endpoint, const std::vector<std::uint8_t>& report) {
    if (endpoint != 0x81 || report.size() != 7) return {ERROR_INVALID_DATA,"Unexpected report"};
    Fake::sending = true;
    const unsigned call = ++Fake::sendCalls;
    std::unique_lock<std::mutex> lock(Fake::mutex);
    Fake::wake.wait(lock, [=] { return !Fake::block && Fake::pauseAt != call; });
    if (Fake::sendError) return {Fake::sendError, "Offline send failed"};
    Fake::reports.push_back(report); return {};
}
Output Get_Output(std::uint32_t timeout) {
    Output value; value.error = timeout == 0 ? ERROR_TIMEOUT : ERROR_INVALID_PARAMETER;
    value.timed_out = timeout == 0; return value;
}
Result Respond_Output(const Output&, const std::vector<std::uint8_t>&, bool) { return {}; }
}
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void Until(Function function, const char* message, int milliseconds = 2500) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!function()) {
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ++checks;
}
void Ready(VirtualMouse::Session& session) {
    session.Start(nullptr);
    Until([&] { return session.Read().ready; }, "Mouse did not become ready");
}
}
int main() {
    try {
        using namespace VirtualMouse;
        Fake::Reset();
        {
            Session session; Fake::infoError = ERROR_FILE_NOT_FOUND; session.Start(nullptr);
            Until([&] { return !session.Read().starting; }, "Missing-driver failure did not finish");
            const auto status = session.Read();
            Check(status.error == ERROR_FILE_NOT_FOUND && !status.ready, "Missing driver was not reported");
            Check(Fake::creates == 0, "Missing driver still attempted device creation");
        }
        Fake::Reset();
        {
            Session session; Fake::configured = false; session.Start(nullptr);
            Until([] { return Fake::creates == 1; }, "Creation did not run");
            Check(!session.Send({200}), "Unconfigured mouse accepted input");
            Fake::configured = true;
            Until([&] { return session.Read().ready; }, "Delayed enumeration did not become ready");
            Check(session.Send({-32768,32767}), "Signed movement boundaries were rejected");
            Until([&] { return session.Read().reports == 1; }, "Movement did not reach transport");
            Check(!session.Send({32768}), "Out-of-range movement was accepted");
            Check(session.Read().error == ERROR_INVALID_PARAMETER, "Invalid movement lacks an error");
            session.Stop();
            Check(Fake::deletes == 1 && !Fake::created, "Stop did not remove the owned mouse");
            std::lock_guard<std::mutex> lock(Fake::mutex);
            Check(Fake::reports.size() == 2 && Fake::reports.back() == Report({}), "Stop did not send a neutral release");
        }
        Fake::Reset();
        {
            Session session; Ready(session);
            Check(session.Click(2), "Ready mouse rejected a click");
            Check(!session.Click(2), "Two clicks were armed simultaneously");
            Check(session.Read().click_pending, "Armed click is not visible");
            Until([&] { return !session.Read().click_pending; }, "Click did not finish");
            std::lock_guard<std::mutex> lock(Fake::mutex);
            Check(Fake::reports.empty() || (Fake::reports.size() == 2 && Fake::reports[0][0] == 2 && Fake::reports[1][0] == 0), "Click did not preserve press/release edges");
        }
        Fake::Reset();
        {
            Session session; Ready(session);
            Fake::block = true;
            Check(session.Send({1}), "Initial queue command failed");
            Until([] { return Fake::sending.load(); }, "Send did not enter offline transport");
            unsigned accepted = 0;
            for (unsigned i = 0; i < 100; ++i) accepted += session.Send({1});
            Check(accepted == 64, "Input queue is not bounded to 64 commands");
            Fake::block = false; Fake::wake.notify_all();
            Until([&] { return session.Read().reports == 65; }, "Queued burst lost reports");
            Check(session.Read().last.x == 1, "Last accepted input state is wrong");
        }
        Fake::Reset();
        {
            Session session; Ready(session); Fake::block = true;
            Check(session.Send({1}), "Shutdown probe was not queued");
            Until([] { return Fake::sendCalls == 1; }, "Shutdown probe did not enter transport");
            for (unsigned i = 0; i < 64; ++i) Check(session.Send({1}), "Shutdown burst queue unexpectedly rejected a command");
            Fake::pauseAt = 3; Fake::block = false; Fake::wake.notify_all();
            Until([] { return Fake::sendCalls == 3; }, "Worker did not enter its local command batch");
            std::thread closing([&] { session.Stop(); });
            Until([&] { return !session.Send({1}); }, "Stop did not reject new commands");
            Fake::pauseAt = 0; Fake::wake.notify_all(); closing.join();
            Check(Fake::sendCalls == 4, "Stop replayed the rest of an already dequeued batch");
            Check(Fake::deletes == 1, "Shutdown burst did not remove the owned mouse");
        }
        Fake::Reset();
        {
            Session session; Ready(session); Fake::expired = true;
            Check(session.Send({1}), "Expiry probe command was not queued");
            Until([&] { return !session.Read().ready; }, "Expiry did not stop the session");
            session.Stop();
            Check(session.Read().trial_expired && session.Read().error == ERROR_ACCOUNT_RESTRICTION, "Expiry is not explained");
            Check(Fake::deletes == 1, "Expiry did not remove the owned mouse");
        }
        Fake::Reset();
        {
            Session session; Ready(session); Fake::quiescing = true; session.Send({1});
            Until([&] { return !session.Read().ready; }, "Quiescing did not stop the session");
            session.Stop(); Check(Fake::deletes == 1, "Quiescing did not remove the owned mouse");
        }
        Fake::Reset();
        {
            Session session; Ready(session); Fake::sendError = ERROR_DEVICE_NOT_CONNECTED; session.Send({1});
            Until([&] { return !session.Read().ready; }, "Send failure did not stop the session");
            session.Stop();
            Check(session.Read().error == ERROR_DEVICE_NOT_CONNECTED && Fake::deletes == 1, "Failed send lost its error or ownership cleanup");
        }
        Fake::Reset();
        {
            Session session; Fake::throwInfo = true; session.Start(nullptr);
            Until([&] { return !session.Read().starting; }, "Worker exception did not finish");
            Check(session.Read().error == ERROR_UNHANDLED_EXCEPTION, "Worker exception escaped instead of becoming a diagnostic");
        }
        std::cout << checks << " offline session checks passed. No driver or inputs were used.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
