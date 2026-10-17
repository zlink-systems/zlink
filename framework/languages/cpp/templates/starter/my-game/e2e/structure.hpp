#pragma once
#include "server/shared/config.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace MyGame::E2E
{
enum class Scenario
{
    connect
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
// Add application scenarios here and in zlink.structure.json together.
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::connect, "connect"}};
inline std::vector<std::string> scenario_names ()
{
    std::vector<std::string> result;
    for (const auto &scenario : scenario_order)
        result.emplace_back (scenario.name);
    return result;
}
inline void require (bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error (message);
}
inline std::string nonempty (const nlohmann::json &j, const char *field)
{
    const auto value = j.at (field).get<std::string> ();
    require (!value.empty (), std::string ("Empty ") + field);
    return value;
}
inline std::filesystem::path project_path (const std::filesystem::path &root,
                                           const std::string &value)
{
    const std::filesystem::path path (value);
    require (!value.empty () && value.find ('\\') == std::string::npos && !path.is_absolute ()
               && !path.has_root_name (),
             "Configuration path must be relative");
    for (const auto &part : path)
        require (part != ".." && part != ".", "Configuration path traversal");
    const auto actual = std::filesystem::weakly_canonical (root / path);
    const auto relative = actual.lexically_relative (std::filesystem::canonical (root));
    require (!relative.empty () && *relative.begin () != ".."
               && std::filesystem::is_regular_file (actual),
             "Configuration outside project or missing");
    return actual;
}
inline void check_structure (const std::filesystem::path &root)
{
    const auto j = read_json (root / "zlink.structure.json");
    require (j.at ("version") == 1, "Unknown structure version");
    (void) nonempty (j, "id");
    (void) nonempty (j, "name");
    require (nonempty (j, "language") == "cpp", "Expected C++ structure");
    const auto &run = j.at ("run");
    require (run.at ("scenarios").get<std::vector<std::string>> () == scenario_names (),
             "Scenario order mismatch");
    const auto &commands = run.at ("commands");
    require (commands.is_object () && commands.size () == 2 && commands.contains ("sh")
               && commands.contains ("powershell"),
             "Invalid run commands");
    for (const auto *shell : {"sh", "powershell"}) {
        require (commands.at (shell).is_array () && !commands.at (shell).empty (),
                 "Empty command list");
        for (const auto &command : commands.at (shell))
            require (command.is_string () && !command.get<std::string> ().empty (),
                     "Empty command");
    }
    std::set<std::string> nodes;
    std::map<std::string, std::string> roles;
    require (j.at ("nodes").is_array () && j.at ("spots").is_array ()
               && j.at ("channels").is_array () && j.at ("edges").is_array (),
             "Structure collections must be arrays");
    std::size_t applications = 0;
    for (const auto &node : j.at ("nodes")) {
        const auto id = nonempty (node, "id"), role = nonempty (node, "role");
        roles[id] = role;
        (void) nonempty (node, "title");
        require (nodes.insert (id).second, "Duplicate node ID");
        if (role == "external")
            require (node.at ("config").is_null (), "External config must be null");
        else {
            const auto path = project_path (root, nonempty (node, "config"));
            if (role == "location-store")
                continue;
            require (role == Names::session, "No entry point is registered for this node role");
            require (NodeConfig::load (path).Id == id, "Config ID mismatch");
            ++applications;
        }
    }
    require (applications > 0, "At least one application node must be declared");
    require (j.at ("channels").empty (), "No application channels are registered");
    require (j.at ("spots").empty (), "No application Spots are registered");
    for (const auto &edge : j.at ("edges")) {
        require (nodes.contains (nonempty (edge, "from")) && nodes.contains (nonempty (edge, "to")),
                 "Unknown edge endpoint");
        (void) nonempty (edge, "label");
    }
}
}
