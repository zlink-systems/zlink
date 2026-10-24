#pragma once
#include "process.hpp"
#include <random>
#include <iostream>
#include <cstdlib>

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
    static constexpr int run_room_capacity = 1;
    struct Node
    {
        std::unique_ptr<Process> process;
        bool stopped = false;
    };
    std::filesystem::path root_, bin_, logs_;
    std::string project_, redis_;
    std::string docker_project_;
    std::vector<std::string> containers_;
    static std::string environment (const char *name)
    {
        const auto *value = std::getenv (name);
        return value ? value : "";
    }
    std::vector<std::unique_ptr<Node>> nodes_;
    std::map<std::string, Node *> active_;
    std::map<std::string, std::pair<std::string, std::string>> definitions_;
    bool compose_started_ = false;
    int command_index_ = 0;
    std::vector<std::string> compose (std::initializer_list<std::string> arguments) const
    {
        std::vector<std::string> result{"docker",         "compose",
                                        "--project-name", project_,
                                        "--file",         (root_ / "docker-compose.yml").string ()};
        result.insert (result.end (), arguments);
        return result;
    }
    std::string run_command (const std::vector<std::string> &arguments)
    {
        const auto name = "command-" + std::to_string (++command_index_);
        Process process (arguments, logs_ / (name + ".out.log"), logs_ / (name + ".err.log"));
        require (process.wait_exit (Limits::startup) == 0,
                 "Command failed: " + arguments.front () + "; log " + name);
        return process.output ();
    }
    std::pair<Node *, nlohmann::json> start_node (const std::string &name, bool replacement = false)
    {
        const auto &[role, path] = definitions_.at (name);
        auto config = read_json (root_ / path);
        config["RedisEndpoint"] = redis_;
        config["KeyPrefix"] = project_ + ":";
        config["MeshEndpoint"] = "tcp://127.0.0.1:0";
        if (role == "match")
            config["RoomCapacity"] = run_room_capacity;
        if (role == "session")
            config["StreamEndpoint"] = "tcp://127.0.0.1:0";
        if (replacement && config.contains ("BootstrapMatchmaking"))
            config["BootstrapMatchmaking"] = false;
        const auto suffix = name + (replacement ? "-replacement" : "");
        const auto config_path = logs_ / (suffix + ".json");
        {
            std::ofstream output (config_path);
            output << config.dump (2);
            require (static_cast<bool> (output), "Config write failed");
        }
        auto node = std::make_unique<Node> ();
        auto executable = bin_ / ("my-game-" + role);
#ifdef _WIN32
        executable += ".exe";
#endif
        std::vector<std::string> arguments{executable.string (), config_path.string ()};
        if (!docker_project_.empty ()) {
            const auto container = docker_project_ + "-" + logs_.filename ().string () + "-" + suffix;
            constexpr auto container_config = "/app/node.json";
            const auto image = environment ("MY_GAME_NODE_IMAGE");
            const auto runner = environment ("MY_GAME_RUNNER_CONTAINER");
            require (!image.empty () && !runner.empty (), "Missing Docker runner settings");
            run_command ({"docker", "create", "-i", "--name", container, "--label",
                          "my-game.e2e=" + docker_project_, "--network", "container:" + runner,
                          image, "/app/bin/my-game-" + role, container_config});
            containers_.push_back (container);
            run_command ({"docker", "cp", config_path.string (), container + ":" + container_config});
            arguments = {"docker", "start", "-a", "-i", container};
        }
        node->process = std::make_unique<Process> (
          arguments, logs_ / (suffix + ".out.log"), logs_ / (suffix + ".err.log"));
        auto *result = node.get ();
        nodes_.push_back (std::move (node));
        active_[config.at ("Id").get<std::string> ()] = result;
        const auto line = result->process->wait_line (Control::ready);
        const auto ready =
          nlohmann::json::parse (line.substr (std::string (Control::ready).size ()));
        require (ready.at ("id") == config.at ("Id"), "Node readiness ID mismatch");
        const auto mesh = ready.at ("meshEndpoint").get<std::string> ();
        require (!mesh.empty () && !mesh.ends_with (":0"),
                 "Framework did not publish the bound mesh endpoint");
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
        project_ ("my-game-" + unique_id ()),
        docker_project_ (environment ("MY_GAME_DOCKER_PROJECT"))
    {
        logs_ = root_ / ".logs" / ("run-" + project_);
        std::filesystem::create_directories (logs_);
        std::cout << "Logs: " << logs_.string () << std::endl;
    }
    std::filesystem::path client_log (std::size_t index) const
    {
        return logs_ / ("client-" + std::to_string (index) + ".jsonl");
    }
    std::string start ()
    {
        if (docker_project_.empty ()) {
            compose_started_ = true;
            run_command (compose ({"up", "--detach", "--wait", "redis"}));
            auto endpoint = run_command (compose ({"port", "redis", "6379"}));
            endpoint.erase (endpoint.find_last_not_of ("\r\n ") + 1);
            require (endpoint.starts_with ("127.0.0.1:") && !endpoint.ends_with (":0"),
                     "Redis ephemeral endpoint missing");
            redis_ = endpoint;
        } else {
            redis_ = "redis:6379";
        }
        std::vector<std::pair<int, std::string>> order;
        const auto structure = read_json (root_ / "zlink.structure.json");
        for (const auto &node : structure.at ("nodes")) {
            const auto role = node.at ("role").get<std::string> ();
            if (role != "match" && role != "session")
                continue;
            const auto id = node.at ("id").get<std::string> ();
            definitions_[id] = {role, node.at ("config").get<std::string> ()};
            const auto rank = role == "session" ? 2
                                                : (read_json (root_ / definitions_.at (id).second)
                                                       .value ("BootstrapMatchmaking", false)
                                                     ? 0
                                                     : 1);
            order.emplace_back (rank, id);
        }
        std::stable_sort (order.begin (), order.end (),
                          [] (const auto &a, const auto &b) { return a.first < b.first; });
        nlohmann::json ready;
        for (const auto &[rank, id] : order) {
            const auto started = start_node (id);
            if (definitions_.at (id).first == "session") {
                require (ready.is_null (), "E2E requires one Session node");
                ready = started.second;
            }
        }
        require (!ready.is_null (), "Session node missing");
        const auto stream = ready.at ("streamEndpoint").get<std::string> ();
        require (!stream.empty () && !stream.ends_with (":0"),
                 "Framework did not publish stream endpoint");
        return stream;
    }
    Process &node (const std::string &id) { return *active_.at (id)->process; }
    void replace (const std::string &id)
    {
        auto *old = active_.at (id);
        stop_node (*old);
        start_node (id, true);
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
        for (const auto &container : containers_) {
            try {
                run_command ({"docker", "rm", "-f", container});
            }
            catch (const std::exception &error) {
                failures.push_back (error.what ());
            }
        }
        containers_.clear ();
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
