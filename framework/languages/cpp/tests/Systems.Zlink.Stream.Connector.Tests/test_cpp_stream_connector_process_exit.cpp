/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <zlink/stream_connector/contracts/zlink_stream_connector_factory.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace
{

int run_child ()
{
    WSADATA data{};
    if (WSAStartup (MAKEWORD (2, 2), &data) != 0)
        return 2;
    const auto listener = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return 3;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    if (bind (listener, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0
        || listen (listener, 1) != 0)
        return 4;
    int length = sizeof (address);
    if (getsockname (listener, reinterpret_cast<sockaddr *> (&address), &length) != 0)
        return 5;
    std::thread ([listener] {
        const auto peer = accept (listener, nullptr, nullptr);
        if (peer != INVALID_SOCKET) {
            char byte;
            (void) recv (peer, &byte, 1, 0);
            closesocket (peer);
        }
        closesocket (listener);
    }).detach ();

    zlink::stream_connector::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (ntohs (address.sin_port));
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    std::atomic_bool callback_entered{false};
    auto subscription = connector.on_connection_state_changed ([&] (const auto &event) {
        if (event.current == zlink::stream_connector::connection_state_t::connected) {
            callback_entered.store (true, std::memory_order_release);
            std::this_thread::sleep_for (std::chrono::seconds (2));
        }
    });
    if (!connector.connect ())
        return 6;
    while (!callback_entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    return 0;
}

int check_child_exit ()
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW (nullptr, path, MAX_PATH) == 0)
        return 7;
    std::wstring command = L"\"" + std::wstring (path) + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW (path, command.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &process))
        return 8;
    const auto wait = WaitForSingleObject (process.hProcess, 8000);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0)
        (void) GetExitCodeProcess (process.hProcess, &exit_code);
    else {
        (void) TerminateProcess (process.hProcess, 9);
        (void) WaitForSingleObject (process.hProcess, INFINITE);
    }
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);
    return wait == WAIT_OBJECT_0 ? static_cast<int> (exit_code) : 9;
}

} // namespace

int main (int argc, char **)
{
    return argc > 1 ? run_child () : check_child_exit ();
}
