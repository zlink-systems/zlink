#pragma once
#include "server/shared/config.hpp"
#include <set>
#include <array>

namespace MyGame::E2E
{
enum class Scenario
{
    cross,
    border,
    reconnect,
    update
};
struct ScenarioDefinition
{
    Scenario id;
    const char *name;
};
inline constexpr std::array scenario_order{ScenarioDefinition{Scenario::cross, "cross"},
                                           ScenarioDefinition{Scenario::border, "border"},
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
    std::set<std::pair<std::string, std::string>> channels;
    for (const auto &channel : j.at ("channels")) {
        const auto name = nonempty (channel, "name"), owner = nonempty (channel, "owner");
        require (nodes.contains (owner) && configurations.contains (owner),
                 "Unknown channel owner");
        require (name == Names::border && nonempty (channel, "mesh") == Names::mesh,
                 "Unknown server channel or mesh");
        require (channels.emplace (name, owner).second, "Duplicate channel registration");
    }
    std::map<std::string, std::set<std::string>> zones;
    for (const auto &spot : j.at ("spots")) {
        require (spot_ids.insert (nonempty (spot, "id")).second, "Duplicate composition Spot ID");
        const auto owner = nonempty (spot, "owner"), kind = nonempty (spot, "kind");
        require (nodes.contains (owner) && configurations.contains (owner), "Unknown Spot owner");
        if (kind == "entry") {
            require (spot.at ("type").is_null () && !spot.contains ("logicalId")
                       && !spot.contains ("dynamic"),
                     "Entry Spot cannot declare type or logical ID");
            require (configurations.at (owner).contains ("ZoneIds"),
                     "Entry owner must register WorldEntry");
            continue;
        }
        require (kind == "user", "Unknown application Spot kind");
        const auto type = nonempty (spot, "type");
        require (type == Names::zone || type == Names::dungeon, "Unknown stable type");
        if (spot.contains ("dynamic"))
            require (spot.at ("dynamic").is_boolean (), "Dynamic must be boolean");
        if (spot.value ("dynamic", false))
            require (!spot.contains ("logicalId"), "Dynamic Spot cannot declare logical ID");
        else {
            const auto id = nonempty (spot, "logicalId");
            require (logical_ids.insert (id).second, "Duplicate logical Spot ID");
            if (type == Names::zone)
                zones[owner].insert (id);
        }
        if (type == Names::zone)
            require (configurations.at (owner).contains ("ZoneIds")
                       && channels.contains ({Names::border, owner}),
                     "Zone owner must register Zone and border");
        else
            require (!configurations.at (owner).contains ("ZoneIds")
                       && !configurations.at (owner).contains ("StreamEndpoint"),
                     "Dungeon owner must register Dungeon");
    }
    for (const auto &[id, config] : configurations) {
        if (!config.contains ("ZoneIds"))
            continue;
        const auto ids = config.at ("ZoneIds").get<std::vector<std::string>> ();
        const std::set<std::string> configured (ids.begin (), ids.end ());
        require (configured.size () == ids.size () && configured == zones[id],
                 "Configured ZoneIds do not match fixed Zones");
        require (config.at ("ZoneCapacity").get<int> () >= static_cast<int> (ids.size ()),
                 "Zone capacity smaller than initial placement");
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
