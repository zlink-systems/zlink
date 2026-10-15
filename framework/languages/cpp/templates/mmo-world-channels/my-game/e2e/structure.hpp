#pragma once
#include "server/shared/config.hpp"
#include <set>
#include <map>
#include <array>

namespace MyGame::E2E
{
enum class Scenario
{
    worlds,
    channel,
    reconnect,
    update
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::worlds, "worlds"},
                                           ScenarioDefinition{Scenario::channel, "channel"},
                                           ScenarioDefinition{Scenario::reconnect, "reconnect"},
                                           ScenarioDefinition{Scenario::update, "update"}};
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
    std::set<std::string> nodes, spot_ids;
    std::map<std::string, nlohmann::json> configurations;
    require (j.at ("nodes").is_array () && j.at ("spots").is_array ()
               && j.at ("channels").is_array () && j.at ("edges").is_array (),
             "Structure collections must be arrays");
    for (const auto &node : j.at ("nodes")) {
        const auto id = nonempty (node, "id"), role = nonempty (node, "role");
        (void) nonempty (node, "title");
        require (nodes.insert (id).second, "Duplicate node ID");
        if (role == "external")
            require (node.at ("config").is_null (), "External config must be null");
        else {
            const auto path = project_path (root, nonempty (node, "config"));
            if (role != "world-db") {
                configurations[id] = read_json (path);
                require (configurations[id].at ("Id") == id, "Config ID mismatch");
                if (role == "accounts") {
                    const auto accounts = configurations[id];
                    require (!nonempty (accounts, "HttpEndpoint").empty ()
                               && !accounts.at ("Worlds").empty (),
                             "Accounts configuration required");
                } else {
                    require (role == "session" || role == "channel", "Unknown application role");
                    const auto config = NodeConfig::load (path);
                    require ((role == "session") == config.StreamEndpoint.has_value (),
                             "Role and STREAM listener differ");
                    require ((role == "channel") == !config.ZoneIds.empty (),
                             "Role and Zone registration differ");
                }
            }
        }
    }
    require (j.at ("channels").empty (), "World channels are User Spots, not named channels");
    std::set<std::pair<std::string, std::string>> identities;
    std::map<std::string, std::set<std::string>> zones;
    for (const auto &spot : j.at ("spots")) {
        require (spot_ids.insert (nonempty (spot, "id")).second, "Duplicate composition Spot ID");
        const auto owner = nonempty (spot, "owner"), kind = nonempty (spot, "kind");
        require (configurations.contains (owner), "Unknown Spot owner");
        const auto &config = configurations.at (owner);
        require (config.contains ("ZoneIds"), "Spot owner must be a channel node");
        if (kind == "entry") {
            require (spot.at ("type").is_null () && !spot.contains ("logicalId")
                       && !spot.contains ("dynamic"),
                     "Entry Spot cannot declare type or logical ID");
            continue;
        }
        require (kind == "user" && nonempty (spot, "type") == Names::zone, "Invalid Zone type");
        require (!spot.value ("dynamic", false), "World Zones must be fixed");
        const auto mesh = nonempty (spot, "mesh"), id = nonempty (spot, "logicalId");
        require (mesh == Names::mesh (config.at ("WorldId")), "Zone mesh differs from owner world");
        (void) Names::channel_of (id);
        require (identities.emplace (mesh, id).second, "Duplicate logical Spot identity in mesh");
        zones[owner].insert (id);
    }
    for (const auto &[id, config] : configurations) {
        if (!config.contains ("ZoneIds"))
            continue;
        const auto ids = config.at ("ZoneIds").get<std::vector<std::string>> ();
        const std::set<std::string> configured (ids.begin (), ids.end ());
        require (configured.size () == ids.size () && configured == zones[id],
                 "Configured Zones differ from structure");
        require (config.at ("ZoneCapacity").get<int> () >= static_cast<int> (ids.size ()),
                 "Insufficient Zone capacity");
    }
    for (const auto &edge : j.at ("edges")) {
        require (nodes.contains (nonempty (edge, "from")) && nodes.contains (nonempty (edge, "to")),
                 "Unknown edge endpoint");
        (void) nonempty (edge, "label");
        if (edge.contains ("semantic"))
            (void) nonempty (edge, "semantic");
    }
}
}
