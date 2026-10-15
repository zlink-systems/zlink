#pragma once
#include "server/shared/config.hpp"
#include <set>
#include <array>
#include <map>
#include <algorithm>

namespace MyGame::E2E
{
enum class Scenario
{
    hit,
    reconnect,
    update
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::hit, "hit"},
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
    std::set<std::string> nodes, spot_ids, logical_ids;
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
            if (role != "location-store") {
                configurations[id] = read_json (path);
                require (configurations[id].at ("Id") == id, "Config ID mismatch");
                (void) NodeConfig::load (path);
            }
        }
    }
    require (j.at ("channels").empty (), "This server does not register named channels");
    std::set<std::string> bootstrap_owners;
    for (const auto &spot : j.at ("spots")) {
        require (spot_ids.insert (nonempty (spot, "id")).second, "Duplicate composition Spot ID");
        const auto owner = nonempty (spot, "owner"), kind = nonempty (spot, "kind");
        require (nodes.contains (owner) && configurations.contains (owner), "Unknown Spot owner");
        if (kind == "entry") {
            require (spot.at ("type").is_null () && !spot.contains ("logicalId")
                       && !spot.contains ("dynamic"),
                     "Entry Spot cannot declare type or logical ID");
            require (configurations.at (owner).contains ("RoomCapacity"),
                     "Entry owner must register Entry");
            continue;
        }
        require (kind == "user", "Unknown application Spot kind");
        const auto type = nonempty (spot, "type");
        require (type == Names::room || type == Names::matchmaking, "Unknown stable type");
        require (configurations.at (owner).contains ("RoomCapacity"),
                 "Spot owner must register match types");
        if (spot.contains ("dynamic"))
            require (spot.at ("dynamic").is_boolean (), "Dynamic must be boolean");
        if (spot.value ("dynamic", false)) {
            require (!spot.contains ("logicalId") && type == Names::room, "Invalid dynamic Spot");
        } else {
            const auto id = nonempty (spot, "logicalId");
            require (logical_ids.insert (id).second, "Duplicate logical Spot ID");
            require (type == Names::matchmaking && id == Names::matchmaking_id,
                     "Unknown fixed Spot");
            require (configurations.at (owner).value ("BootstrapMatchmaking", false),
                     "Matchmaking owner must bootstrap");
            bootstrap_owners.insert (owner);
        }
    }
    for (const auto &[id, config] : configurations) {
        if (config.contains ("RoomCapacity")) {
            require (config.at ("RoomCapacity").get<int> () > 0, "Room capacity must be positive");
            require (config.value ("BootstrapMatchmaking", false) == bootstrap_owners.contains (id),
                     "Bootstrap differs from fixed Spot ownership");
        }
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
