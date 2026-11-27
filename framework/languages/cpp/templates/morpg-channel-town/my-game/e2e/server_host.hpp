#pragma once

#include "world.hpp"
#include <atomic>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <csignal>

namespace MyGame::E2E
{
namespace ipc = boost::interprocess;

inline auto server_state_path (const std::filesystem::path &root)
{
    return root / ".logs/server.json";
}

inline bool server_running (const std::filesystem::path &root)
{
    const auto state = server_state_path (root);
    if (!std::filesystem::is_regular_file (state))
        return false;
    ipc::file_lock lock ((state.parent_path () / "server.lock").string ().c_str ());
    ipc::scoped_lock guard (lock, ipc::try_to_lock);
    return !guard.owns () && std::filesystem::is_regular_file (state);
}

inline auto running_server (const std::filesystem::path &root)
{
    require (server_running (root), "Start the server with run-server before attaching.");
    const auto state = read_json (server_state_path (root));
    std::cout << "Server logs: " << state.at ("logs").get<std::string> () << std::endl;
    return state;
}

inline std::atomic<bool> stop_server{false};
static_assert (std::atomic<bool>::is_always_lock_free);
#ifdef _WIN32
inline BOOL WINAPI on_console_stop (DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    stop_server.store (true);
    return TRUE;
}
#else
inline void on_server_stop (int)
{
    stop_server.store (true);
}
#endif

inline void log_failure (const std::filesystem::path &logs,
                         const std::string &scenario,
                         const std::exception &error)
{
    const auto file = logs / "failure.log";
    std::ofstream output (file, std::ios::app);
    if (!scenario.empty ())
        output << scenario << ": ";
    output << error.what () << '\n';
    output.close ();
    if (!output) {
        std::cerr << "Cannot write " << file.string () << '\n' << error.what () << std::endl;
        return;
    }
    const std::string message = error.what ();
    std::cerr << "✗ " << (scenario.empty () ? "Run" : scenario) << ": "
              << message.substr (0, message.find_first_of ("\r\n")) << " (" << file.string () << ")"
              << std::endl;
}

inline void run_server (const std::filesystem::path &root, const std::filesystem::path &bin)
{
    const auto state = server_state_path (root);
    std::filesystem::create_directories (state.parent_path ());
    const auto lock_path = state.parent_path () / "server.lock";
    std::ofstream (lock_path, std::ios::app).close ();
    ipc::file_lock lock (lock_path.string ().c_str ());
    ipc::scoped_lock guard (lock, ipc::try_to_lock);
    require (guard.owns (), "A server is already starting or running. Stop it with Ctrl+C.");
    std::filesystem::remove (state);
#ifdef _WIN32
    require (SetConsoleCtrlHandler (on_console_stop, TRUE), "Cannot register console stop handler");
#else
    std::signal (SIGINT, on_server_stop);
    std::signal (SIGTERM, on_server_stop);
#endif
    World world (root, bin);
    auto temporary = state;
    temporary += ".tmp";
    bool failed = false;
    try {
        auto endpoints = world.start_server ();
        if (!stop_server.load ()) {
            endpoints["logs"] = world.logs ().string ();
            std::ofstream output (temporary);
            output << endpoints.dump (2) << '\n';
            output.close ();
            require (static_cast<bool> (output), "Cannot write " + temporary.string ());
            std::filesystem::rename (temporary, state);
            std::cout << "Server ready: " << world.client_addresses (endpoints)
                      << " (game clients connect here)\nLogs: " << world.logs ().string ()
                      << std::endl;
            while (!stop_server.load ())
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
    }
    catch (const std::exception &error) {
        log_failure (world.logs (), {}, error);
        failed = true;
    }
    std::error_code state_error, temporary_error;
    std::filesystem::remove (state, state_error);
    std::filesystem::remove (temporary, temporary_error);
    world.close ();
    require (!state_error && !temporary_error, "Cannot remove server metadata");
    require (!failed, "Server startup failed");
}
}
