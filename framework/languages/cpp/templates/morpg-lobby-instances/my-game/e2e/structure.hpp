#pragma once
#include "server/shared/config.hpp"
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace MyGame::E2E
{
enum class Scenario
{
    party,
    reconnect,
    update
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::party, "party"},
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
            if (role != "location-store") {
                configurations[id] = read_json (path);
                require (configurations[id].at ("Id") == id, "Config ID mismatch");
                (void) NodeConfig::load (path);
            }
        }
    }
    require (j.at ("id") == "morpg-lobby-instances" && j.at ("name") == "my-game",
             "Structure identity mismatch");
    const std::map<std::string, std::string> roles{
      {"players", "external"},   {"session", "session"},    {"lobby", "lobby"},
      {"instanceA", "instance"}, {"instanceB", "instance"}, {"location", "location-store"}};
    require (j.at ("nodes").size () == roles.size (), "Node count mismatch");
    for (const auto &node : j.at ("nodes"))
        require (roles.contains (node.at ("id")) && node.at ("role") == roles.at (node.at ("id")),
                 "Node role mismatch");
    require (j.at ("channels").empty (), "This structure has no channels");
    require (j.at ("spots").size () == 3, "Spot count mismatch");
    for (const auto &spot : j.at ("spots")) {
        const auto id = nonempty (spot, "id"), owner = nonempty (spot, "owner"),
                   kind = nonempty (spot, "kind");
        require (spot_ids.insert (id).second && configurations.contains (owner),
                 "Invalid Spot owner or duplicate ID");
        if (id == "entry")
            require (kind == "entry" && owner == "lobby" && spot.at ("type").is_null ()
                       && !spot.contains ("logicalId") && !spot.contains ("dynamic"),
                     "Invalid Entry Spot");
        else {
            require (kind == "user" && spot.at ("dynamic") == true && !spot.contains ("logicalId"),
                     "Expected dynamic User Spot");
            require (
              (id == "party" && owner == "lobby" && spot.at ("type") == Names::party)
                || (id == "dungeon" && owner == "instanceA" && spot.at ("type") == Names::dungeon),
              "User Spot registration mismatch");
        }
    }
    for (const auto &[id, config] : configurations) {
        require ((id == "session") == config.contains ("StreamEndpoint"),
                 "Session endpoint ownership mismatch");
        const auto instance = roles.at (id) == "instance";
        require (instance == config.contains ("DungeonCapacity"),
                 "Dungeon capacity ownership mismatch");
        if (instance)
            require (config.at ("DungeonCapacity").get<int> () > 0,
                     "Dungeon capacity must be positive");
    }
    std::set<std::tuple<std::string, std::string, std::string, std::string>> edges;
    for (const auto &edge : j.at ("edges")) {
        const auto from = nonempty (edge, "from"), to = nonempty (edge, "to"),
                   label = nonempty (edge, "label");
        require (nodes.contains (from) && nodes.contains (to), "Unknown edge endpoint");
        require (edges.emplace (from, to, label, edge.value ("semantic", std::string{})).second,
                 "Duplicate edge");
    }
    const decltype (edges) expected{
      {"players", "session", "STREAM", ""},
      {"session", "lobby", "relay", ""},
      {"lobby", "instanceA", "GetOrCreate", ""},
      {"lobby", "location", "Location / lease", "location-authority"}};
    require (edges == expected, "Structure edge mismatch");
    for (const auto *file :
         {"README.md", "AGENTS.md", "CLAUDE.md", "CMakeLists.txt", "CMakePresets.json",
          "vcpkg.json", "run-e2e.sh", "run-e2e.ps1", "server/session/main.cpp",
          "server/lobby/main.cpp", "server/instance/main.cpp", "e2e/main.cpp"})
        (void) project_path (root, file);
}
}
