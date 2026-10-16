#pragma once
#include "process.hpp"
#include <random>
#include <iostream>
namespace MyGame::E2E
{
inline std::string unique_id ()
{
    std::ostringstream out;
    out << std::hex << std::chrono::system_clock::now ().time_since_epoch ().count ()
        << std::random_device{}();
    return out.str ();
}
class World
{
    struct Node
    {
        std::unique_ptr<Process> process;
        bool stopped = false;
    };
    std::filesystem::path root_, bin_, logs_;
    std::string project_, accounts_;
    std::map<std::string, std::string> redis_, endpoints_;
    std::vector<std::unique_ptr<Node>> nodes_;
    Node *channel_a1_ = nullptr;
    bool compose_started_ = false;
    int command_index_ = 0;
    std::vector<std::string> compose (std::initializer_list<std::string> args) const
    {
        std::vector<std::string> result{"docker",         "compose",
                                        "--project-name", project_,
                                        "--file",         (root_ / "docker-compose.yml").string ()};
        result.insert (result.end (), args);
        return result;
    }
    std::string run_command (const std::vector<std::string> &args)
    {
        const auto name = "command-" + std::to_string (++command_index_);
        Process process (args, logs_ / (name + ".out.log"), logs_ / (name + ".err.log"));
        require (process.wait_exit (Limits::startup) == 0,
                 "Command failed: " + args.front () + "; log " + name);
        return process.output ();
    }
    std::pair<Node *, nlohmann::json>
    start_node (const std::string &name, const std::string &role, bool replacement = false)
    {
        auto config = read_json (root_ / "server/config" / (name + ".json"));
        if (role == "accounts") {
            config["HttpEndpoint"] = "http://127.0.0.1:0";
            for (auto &world : config.at ("Worlds"))
                world["sessionEndpoint"] = endpoints_.at (world.at ("worldId").get<std::string> ());
        } else {
            const auto world = config.at ("WorldId").get<std::string> ();
            config["RedisEndpoint"] = redis_.at (world);
            config["KeyPrefix"] = project_ + ":world:" + world + ":";
            config["MeshEndpoint"] = "tcp://127.0.0.1:0";
            if (role == "session")
                config["StreamEndpoint"] = "tcp://127.0.0.1:0";
        }
        const auto suffix = name + (replacement ? "-replacement" : "");
        const auto path = logs_ / (suffix + ".json");
        {
            std::ofstream output (path);
            output << config.dump (2);
            require (static_cast<bool> (output), "Config write failed");
        }
        auto node = std::make_unique<Node> ();
        auto executable = bin_ / ("my-game-" + role);
#ifdef _WIN32
        executable += ".exe";
#endif
        node->process =
          std::make_unique<Process> (std::vector<std::string>{executable.string (), path.string ()},
                                     logs_ / (suffix + ".out.log"), logs_ / (suffix + ".err.log"));
        auto *result = node.get ();
        nodes_.push_back (std::move (node));
        const auto line = result->process->wait_line (Control::ready);
        const auto ready =
          nlohmann::json::parse (line.substr (std::string (Control::ready).size ()));
        require (ready.at ("id") == config.at ("Id"), "Node readiness ID mismatch");
        const auto endpoint =
          ready.at (role == "accounts" ? "endpoint" : "meshEndpoint").get<std::string> ();
        require (!endpoint.empty () && !endpoint.ends_with (":0"),
                 "Bound endpoint was not published");
        return {result, ready};
    }
    static void stop_node (Node &node)
    {
        if (node.stopped)
            return;
        node.process->command (Control::shutdown);
        node.process->wait_line (Control::stopped, Limits::shutdown);
        require (node.process->wait_exit () == 0, "Node shutdown exit failed");
        node.stopped = true;
    }

  public:
    World (std::filesystem::path root, std::filesystem::path bin) :
        root_ (std::filesystem::canonical (root)),
        bin_ (std::filesystem::canonical (bin)),
        project_ ("my-game-" + unique_id ())
    {
        logs_ = root_ / ".logs" / ("run-" + project_);
        std::filesystem::create_directories (logs_);
        std::cout << "Logs: " << logs_.string () << std::endl;
    }
    void start ()
    {
        compose_started_ = true;
        run_command (compose ({"up", "--detach", "--wait"}));
        for (const auto *world : {"A", "B"}) {
            auto endpoint = run_command (compose ({"port", std::string ("db") + world, "6379"}));
            endpoint.erase (endpoint.find_last_not_of ("\r\n ") + 1);
            require (endpoint.starts_with ("127.0.0.1:") && !endpoint.ends_with (":0"),
                     "Redis endpoint missing");
            redis_[world] = endpoint;
        }
        channel_a1_ = start_node ("channelA1", "channel").first;
        start_node ("channelA2", "channel");
        start_node ("channelB", "channel");
        for (const auto *world : {"A", "B"}) {
            const auto ready = start_node (std::string ("session") + world, "session").second;
            const auto endpoint = ready.at ("streamEndpoint").get<std::string> ();
            require (!endpoint.empty () && !endpoint.ends_with (":0"),
                     "Missing bound STREAM endpoint");
            endpoints_[world] = endpoint;
        }
        accounts_ = start_node ("accounts", "accounts").second.at ("endpoint");
    }
    const std::string &endpoint (const std::string &world) const { return endpoints_.at (world); }
    const std::string &accounts () const { return accounts_; }
    void drain_channel_a1 ()
    {
        auto &process = *channel_a1_->process;
        process.command (Control::drain);
        process.wait_line (Control::requested, Limits::relocation);
        process.wait_line (Control::relocated, Limits::relocation);
        process.wait_line (Control::safe, Limits::safe);
    }
    void replace_channel_a1 ()
    {
        stop_node (*channel_a1_);
        channel_a1_ = start_node ("channelA1", "channel", true).first;
    }
    void close ()
    {
        std::vector<std::string> failures;
        for (auto node = nodes_.rbegin (); node != nodes_.rend (); ++node) {
            try {
                stop_node (**node);
            }
            catch (const std::exception &error) {
                failures.push_back (error.what ());
                (*node)->process->terminate ();
            }
        }
        nodes_.clear ();
        if (compose_started_) {
            try {
                run_command (compose ({"logs", "--no-color"}));
            }
            catch (const std::exception &error) {
                failures.push_back (error.what ());
            }
            try {
                run_command (compose ({"down", "--volumes", "--remove-orphans"}));
                compose_started_ = false;
            }
            catch (const std::exception &error) {
                failures.push_back (error.what ());
            }
        }
        for (const auto &failure : failures)
            std::cerr << "Cleanup failed: " << failure << std::endl;
        require (failures.empty (), "Run cleanup failed");
    }
};
}
