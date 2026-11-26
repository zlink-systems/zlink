#pragma once
#include "world.hpp"
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <atomic>
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
    ipc::file_lock lock ((root / ".logs/server.lock").string ().c_str ());
    ipc::scoped_lock guard (lock, ipc::try_to_lock);
    return !guard.owns () && std::filesystem::is_regular_file (state);
}

inline void publish_json (const std::filesystem::path &path, const nlohmann::json &value)
{
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output (temporary);
    output << value.dump (2) << '\n';
    output.close ();
    require (static_cast<bool> (output), "Cannot write " + temporary.string ());
    std::filesystem::rename (temporary, path);
}

class AttachedServer
{
    std::string endpoint_;
    RecordsLocation records_;

  public:
    explicit AttachedServer (const std::filesystem::path &root)
    {
        require (server_running (root), "Start the server with run-server before attaching.");
        const auto state = read_json (server_state_path (root));
        endpoint_ = nonempty (state, "sessionEndpoint");
        records_ = {nonempty (state, "redisEndpoint"), nonempty (state, "playerRecordsKeyPrefix")};
    }
    const std::string &endpoint () const { return endpoint_; }
    const RecordsLocation &records_location () const { return records_; }
};

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

inline void serve (World &world, const std::filesystem::path &state)
{
    const auto endpoint = world.start ();
    const auto records = world.records_location ();
    publish_json (state, {{"sessionEndpoint", endpoint},
                          {"run", world.logs ().filename ().string ()},
                          {"redisEndpoint", records.redis},
                          {"playerRecordsKeyPrefix", records.prefix}});
    std::cout << "Server ready: " << endpoint << " (game clients connect here)" << std::endl;
    while (!stop_server.load ())
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
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
    World world (root, bin, 0, true);
    std::exception_ptr failure;
    try {
        serve (world, state);
    }
    catch (const std::exception &) {
        failure = std::current_exception ();
    }
    std::error_code cleanup;
    std::filesystem::remove (state, cleanup);
    try {
        world.close ();
        require (!cleanup, "Cannot remove server state: " + cleanup.message ());
    }
    catch (const std::exception &error) {
        if (!failure)
            throw;
        std::cerr << "Server cleanup failed: " << error.what () << std::endl;
    }
    if (failure)
        std::rethrow_exception (failure);
}
}
