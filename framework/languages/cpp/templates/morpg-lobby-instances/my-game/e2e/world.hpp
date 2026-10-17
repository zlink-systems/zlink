#pragma once
#include "process.hpp"
#include <iostream>
#include <cstdlib>
#include <random>

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
    std::string project_, redis_;
    std::string docker_project_;
    std::vector<std::string> containers_;
    static std::string environment (const char *name)
    {
        const auto *value = std::getenv (name);
        return value ? value : "";
    }
    std::vector<std::unique_ptr<Node>> nodes_;
    std::map<std::string, Node *> instances_;
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
    std::pair<Node *, nlohmann::json>
    start_node (const std::string &name, const std::string &role, bool replacement = false)
    {
        auto config = read_json (root_ / "server/config" / (name + ".json"));
        config["RedisEndpoint"] = redis_;
        config["KeyPrefix"] = project_ + ":";
        config["MeshEndpoint"] = "tcp://127.0.0.1:0";
        if (role == "session")
            config["StreamEndpoint"] = "tcp://127.0.0.1:0";
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
        start_node ("lobby", "lobby");
        instances_["instanceA"] = start_node ("instanceA", "instance").first;
        instances_["instanceB"] = start_node ("instanceB", "instance").first;
        const auto ready = start_node ("session", "session").second;
        const auto stream = ready.at ("streamEndpoint").get<std::string> ();
        require (!stream.empty () && !stream.ends_with (":0"),
                 "Framework did not publish stream endpoint");
        return stream;
    }
    Process &instance (const std::string &id) { return *instances_.at (id)->process; }
    void replace_instance (const std::string &id)
    {
        stop_node (*instances_.at (id));
        instances_[id] = start_node (id, "instance", true).first;
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
