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
    std::filesystem::path control_;
    std::string endpoint_;

    void
    call (const char *operation, const std::string &id, std::chrono::milliseconds timeout) const
    {
        const auto name = unique_id ();
        const auto request = control_ / (name + ".request");
        const auto response = control_ / (name + ".response");
        publish_json (request, {{"operation", operation}, {"node", id}});
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        while (!std::filesystem::exists (response)) {
            require (std::chrono::steady_clock::now () < deadline,
                     std::string (operation) + " control deadline for " + id);
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        const auto result = read_json (response);
        std::filesystem::remove (response);
        const auto error = result.at ("error").get<std::string> ();
        require (error.empty (), error);
    }

  public:
    explicit AttachedServer (const std::filesystem::path &root)
    {
        require (server_running (root), "Start the server with run-server before attaching.");
        const auto state = read_json (server_state_path (root));
        const auto run = nonempty (state, "run");
        constexpr std::string_view run_prefix = "run-my-game-";
        require (run.starts_with (run_prefix) && run.size () > run_prefix.size ()
                   && run.find_first_not_of ("0123456789abcdef", run_prefix.size ())
                        == std::string::npos,
                 "Invalid server run directory");
        control_ = root / ".logs" / run / "control";
        endpoint_ = nonempty (state, "sessionEndpoint");
    }
    const std::string &endpoint () const { return endpoint_; }
    void drain_table (const std::string &id) const
    {
        call ("drain", id, 2 * Limits::relocation + Limits::safe);
    }
    void replace_table (const std::string &id) const
    {
        call ("replace", id, 2 * Limits::shutdown + Limits::startup);
    }
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
    const auto control = world.logs () / "control";
    std::filesystem::create_directory (control);
    publish_json (state,
                  {{"sessionEndpoint", endpoint}, {"run", world.logs ().filename ().string ()}});
    std::cout << "Server ready: " << endpoint << " (game clients connect here)" << std::endl;
    while (!stop_server.load ()) {
        for (const auto &entry : std::filesystem::directory_iterator (control)) {
            if (entry.path ().extension () != ".request")
                continue;
            auto response = entry.path ();
            response.replace_extension (".response");
            std::string error;
            try {
                const auto request = read_json (entry.path ());
                const auto operation = nonempty (request, "operation");
                const auto id = nonempty (request, "node");
                if (operation == "drain")
                    world.drain_table (id);
                else if (operation == "replace")
                    world.replace_table (id);
                else
                    throw std::invalid_argument ("Unknown server control operation");
            }
            catch (const std::exception &failure) {
                error = failure.what ();
                std::cerr << "Server control failed: " << error << std::endl;
            }
            std::filesystem::remove (entry.path ());
            publish_json (response, {{"error", error}});
            if (stop_server.load ())
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
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
