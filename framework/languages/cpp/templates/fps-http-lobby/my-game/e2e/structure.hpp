#pragma once
#include "server/shared/config.hpp"
#include <set>
#include <tuple>
#include <algorithm>
#include <array>

namespace MyGame::E2E
{
enum class Scenario
{
    lobby,
    hit,
    wall,
    input,
    reconnect,
    update
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::lobby, "lobby"},
                                           ScenarioDefinition{Scenario::hit, "hit"},
                                           ScenarioDefinition{Scenario::wall, "wall"},
                                           ScenarioDefinition{Scenario::input, "input"},
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
    std::set<std::string> nodes, spot_ids, logical_ids, match_nodes;
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
                const auto config = NodeConfig::load (path);
                if (role == "session")
                    require (config.StreamEndpoint.has_value (), "Session listener required");
                if (role == "http-lobby")
                    require (!config.HttpEndpoint.empty () && !config.SessionEndpoint.empty (),
                             "HTTP and Session endpoints required");
                if (role == "match") {
                    require (config.RoomCapacity > 0, "Room capacity must be positive");
                    match_nodes.insert (id);
                }
            }
        }
    }
    require (j.at ("channels").empty (), "No named channels are registered");
    for (const auto &spot : j.at ("spots")) {
        require (spot_ids.insert (nonempty (spot, "id")).second, "Duplicate composition Spot ID");
        const auto owner = nonempty (spot, "owner"), kind = nonempty (spot, "kind");
        require (match_nodes.contains (owner) && configurations.contains (owner),
                 "Unknown Spot owner");
        const auto &config = configurations.at (owner);
        require (config.contains ("RoomCapacity") && config.at ("RoomCapacity").get<int> () > 0,
                 "Spot owner must register match server with positive capacity");
        if (kind == "entry") {
            require (spot.at ("type").is_null () && !spot.contains ("logicalId")
                       && !spot.contains ("dynamic"),
                     "Entry Spot cannot declare type or logical ID");
        } else {
            require (kind == "user" && nonempty (spot, "type") == Names::room,
                     "Unknown application Spot kind or type");
            if (spot.contains ("dynamic"))
                require (spot.at ("dynamic").is_boolean (), "Dynamic must be boolean");
            if (spot.value ("dynamic", false))
                require (!spot.contains ("logicalId"), "Dynamic Spot cannot declare logical ID");
            else
                require (logical_ids.insert (nonempty (spot, "logicalId")).second,
                         "Duplicate logical Spot ID");
        }
    }
    const auto &spots = j.at ("spots");
    require (std::any_of (spots.begin (), spots.end (),
                          [] (const auto &spot) {
                              return spot.at ("kind") == "user" && spot.at ("type") == Names::room
                                     && spot.value ("dynamic", false);
                          }),
             "Missing dynamic Room family");
    for (const auto &id : match_nodes)
        require (std::count_if (spots.begin (), spots.end (),
                                [&id] (const auto &spot) {
                                    return spot.at ("owner") == id && spot.at ("kind") == "entry";
                                })
                   == 1,
                 "Room node lacks Entry Spot");
    std::set<std::tuple<std::string, std::string, std::string, std::string>> edges;
    for (const auto &edge : j.at ("edges")) {
        const auto from = nonempty (edge, "from"), to = nonempty (edge, "to"),
                   label = nonempty (edge, "label");
        require (nodes.contains (from) && nodes.contains (to), "Unknown edge endpoint");
        const auto semantic = edge.contains ("semantic") ? nonempty (edge, "semantic") : "";
        edges.emplace (from, to, label, semantic);
    }
    const decltype (edges) required_edges{
      {"players", "lobby", "HTTP", ""},
      {"players", "session", "STREAM", ""},
      {"session", "matchA", "relay", ""},
      {"lobby", "location", "Find room", ""},
      {"matchA", "location", "Location / lease", "location-authority"}};
    require (
      std::includes (edges.begin (), edges.end (), required_edges.begin (), required_edges.end ()),
      "Missing required structure edge");
}
}
