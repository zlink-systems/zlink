#pragma once
#include "server/shared/config.hpp"
#include "server/shared/match_contracts.hpp"
#include "server/shared/player_records.hpp"
#include <algorithm>
#include <set>
#include <tuple>
#include <array>

namespace MyGame::E2E
{
enum class Scenario
{
    table,
    reconnect,
    update,
    match,
    match_cancel,
    persist
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
// Update's placement check needs the other selected scenarios' tables to exist before drain.
inline constexpr std::array scenario_order{
  ScenarioDefinition{Scenario::table, "table"},
  ScenarioDefinition{Scenario::reconnect, "reconnect"},
  ScenarioDefinition{Scenario::match, "match"},
  ScenarioDefinition{Scenario::match_cancel, "match-cancel"},
  ScenarioDefinition{Scenario::persist, "persist"},
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
    require (commands.is_object () && commands.size () == 3 && commands.contains ("sh")
               && commands.contains ("powershell") && commands.contains ("cmd"),
             "Invalid run commands");
    for (const auto *shell : {"sh", "powershell", "cmd"}) {
        require (commands.at (shell).is_array () && !commands.at (shell).empty (),
                 "Empty command list");
        for (const auto &command : commands.at (shell))
            require (command.is_string () && !command.get<std::string> ().empty (),
                     "Empty command");
    }
    std::set<std::string> nodes, spot_ids, logical_ids;
    std::map<std::string, nlohmann::json> configurations;
    std::map<std::string, std::string> roles;
    require (j.at ("nodes").is_array () && j.at ("spots").is_array ()
               && j.at ("channels").is_array () && j.at ("edges").is_array (),
             "Structure collections must be arrays");
    for (const auto &node : j.at ("nodes")) {
        const auto id = nonempty (node, "id"), role = nonempty (node, "role");
        roles[id] = role;
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
    std::set<std::pair<std::string, std::string>> records_roles;
    for (const auto &channel : j.at ("channels")) {
        auto owner = nonempty (channel, "owner");
        require (configurations.contains (owner)
                   && (configurations.at (owner).contains ("ApiEndpoint") == (owner == "api"))
                   && channel.at ("name") == player_records_channel
                   && channel.at ("kind") == "client-server"
                   && records_roles.emplace (std::move (owner), nonempty (channel, "role")).second,
                 "Invalid player records channel declaration");
    }
    const decltype (records_roles) expected_channel{
      {"api", "server"}, {"tableA", "client"}, {"tableB", "client"}};
    require (records_roles == expected_channel, "Missing player records channel role");
    for (const auto &spot : j.at ("spots")) {
        require (spot_ids.insert (nonempty (spot, "id")).second, "Duplicate composition Spot ID");
        const auto owner = nonempty (spot, "owner"), kind = nonempty (spot, "kind");
        require (nodes.contains (owner) && configurations.contains (owner), "Unknown Spot owner");
        if (kind == "entry") {
            require (spot.at ("type").is_null () && !spot.contains ("logicalId")
                       && !spot.contains ("dynamic"),
                     "Entry Spot cannot declare type or logical ID");
            require (roles.at (owner) == "table", "Entry owner must register Table Entry");
            continue;
        }
        const auto type = nonempty (spot, "type");
        if (kind == "instance") {
            const auto id = nonempty (spot, "logicalId");
            require (!spot.value ("dynamic", false)
                       && ((type == MatchNames::pool && roles.at (owner) == "matchmaker"
                            && id == MatchNames::pool_key)
                           || (type == MatchNames::provisioner && roles.at (owner) == "table"
                               && id == MatchNames::provisioner_key)),
                     "Invalid board Instance Spot");
            require (logical_ids.insert (nonempty (spot, "logicalId")).second,
                     "Duplicate logical Spot ID");
            continue;
        }
        require (kind == "user" && type == Names::table, "Unknown application Spot kind or type");
        if (spot.contains ("dynamic"))
            require (spot.at ("dynamic").is_boolean (), "Dynamic must be boolean");
        if (spot.value ("dynamic", false))
            require (!spot.contains ("logicalId"), "Dynamic Spot cannot declare logical ID");
        else {
            const auto id = nonempty (spot, "logicalId");
            require (logical_ids.insert (id).second, "Duplicate logical Spot ID");
        }
        require (roles.at (owner) == "table", "Table owner cannot be a Session process");
    }
    require (logical_ids.contains (MatchNames::pool_key)
               && logical_ids.contains (MatchNames::provisioner_key),
             "Missing Pool or RoomProvisioner");
    const auto &spots = j.at ("spots");
    require (std::any_of (spots.begin (), spots.end (),
                          [] (const auto &spot) {
                              return spot.at ("kind") == "user" && spot.at ("type") == Names::table
                                     && spot.value ("dynamic", false);
                          }),
             "Missing dynamic Table family");
    for (const auto &[id, role] : roles)
        if (role == "table")
            require (std::count_if (spots.begin (), spots.end (),
                                    [&id] (const auto &spot) {
                                        return spot.at ("kind") == "entry"
                                               && spot.at ("owner") == id;
                                    })
                       == 1,
                     "play node (table) must declare exactly one application Entry Spot: " + id);
    std::set<std::tuple<std::string, std::string, std::string, std::string>> edges;
    for (const auto &edge : j.at ("edges")) {
        const auto from = nonempty (edge, "from"), to = nonempty (edge, "to"),
                   label = nonempty (edge, "label");
        require (nodes.contains (from) && nodes.contains (to), "Unknown edge endpoint");
        const auto semantic = edge.contains ("semantic") ? nonempty (edge, "semantic") : "";
        edges.emplace (from, to, label, semantic);
    }
    const decltype (edges) required_edges{
      {"players", "session", "STREAM", ""},
      {"session", "tableA", "relay", ""},
      {"tableA", "matchmaker", "MatchRequest / EnterReport", ""},
      {"matchmaker", "tableA", "PrepareRoom / CommitRoom", ""},
      {"tableA", "matchmaker", "RoomPrepared", ""},
      {"tableA", "api", "loadPlayer / commitRound", ""},
      {"tableB", "api", "loadPlayer / commitRound", ""},
      {"api", "location", "Player records", ""},
      {"matchmaker", "location", "Location / lease", "location-authority"},
      {"tableA", "location", "Location / lease", "location-authority"}};
    require (
      std::includes (edges.begin (), edges.end (), required_edges.begin (), required_edges.end ()),
      "Missing required structure edge");
}
}
