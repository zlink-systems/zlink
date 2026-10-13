using System.Text.Json;
using System.Text.Json.Nodes;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class StructureCheck
{
    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidDataException(message);
    }

    private static string Text(JsonNode? node, string name) =>
        node?[name]?.GetValue<string>() ?? throw new InvalidDataException($"Missing {name}.");

    private static JsonArray Array(JsonObject document, string name) =>
        document[name] as JsonArray ?? throw new InvalidDataException($"Missing {name} array.");

    public static void Run(string root, string? path)
    {
        var document =
            JsonNode.Parse(File.ReadAllText(path ?? Path.Combine(root, "zlink.structure.json")))
                as JsonObject
            ?? throw new InvalidDataException("Structure must be an object.");
        Validate(root, document);
        Console.WriteLine("✓ structure agrees with configs and typed contracts");
        var mutations = new (string Name, Action<JsonObject> Change)[]
        {
            ("unknown version", value => value["version"] = 2),
            (
                "missing config",
                value => value["nodes"]![1]!["config"] = "server/config/missing.json"
            ),
            ("duplicate node", value => value["nodes"]![2]!["id"] = "session"),
            ("outside config", value => value["nodes"]![1]!["config"] = "../outside.json"),
            ("wrong zone owner", value => value["spots"]![2]!["owner"] = "zoneB"),
            ("duplicate spot", value => value["spots"]![3]!["id"] = "zone-1"),
            ("duplicate logical ID", value => value["spots"]![3]!["logicalId"] = "zone-1"),
            ("entry stable type", value => value["spots"]![0]!["type"] = GameNames.ZoneType),
            ("entry dynamic family", value => value["spots"]![0]!["dynamic"] = true),
            ("wrong channel", value => value["channels"]![0]!["name"] = "wrong-border"),
            ("missing edge", value => ((JsonArray)value["edges"]!).RemoveAt(0)),
        };
        foreach (var (name, change) in mutations)
        {
            var bad = (JsonObject)document.DeepClone();
            change(bad);
            try
            {
                Validate(root, bad);
            }
            catch (InvalidDataException)
            {
                Console.WriteLine($"✓ structure rejects {name}");
                continue;
            }
            throw new InvalidDataException($"Structure checker falsely accepted {name}.");
        }
        Console.WriteLine($"Structure: 1 valid input and {mutations.Length} invalid inputs passed");
    }

    private static void Validate(string root, JsonObject document)
    {
        Require(document["version"]?.GetValue<int>() == 1, "Unsupported structure version.");
        Require(
            Text(document, "id") == "mmo-seamless" && Text(document, "language") == "dotnet",
            "Wrong preset or language."
        );
        Require(!string.IsNullOrWhiteSpace(Text(document, "name")), "Project name is required.");
        var nodes = Array(document, "nodes");
        Require(nodes.Count == 6, "Six topology nodes are required.");
        var roles = new Dictionary<string, string>
        {
            ["players"] = "external",
            ["session"] = "session",
            ["zoneA"] = "zone",
            ["zoneB"] = "zone",
            ["instance"] = "instance",
            ["location"] = "location-store",
        };
        var ids = new HashSet<string>();
        var settings = new Dictionary<string, NodeSettings>();
        foreach (var node in nodes)
        {
            var id = Text(node, "id");
            Require(
                ids.Add(id) && roles.TryGetValue(id, out var role) && role == Text(node, "role"),
                "Invalid or duplicate node."
            );
            Require(!string.IsNullOrWhiteSpace(Text(node, "title")), "Node title is required.");
            if (id == "players")
            {
                Require(
                    node!["config"] is null,
                    "External Players must not have server configuration."
                );
                continue;
            }
            var relative = Text(node, "config");
            var config = Path.GetFullPath(Path.Combine(root, relative));
            var prefix =
                Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar)
                + Path.DirectorySeparatorChar;
            Require(
                config.StartsWith(prefix, StringComparison.Ordinal) && File.Exists(config),
                "Configuration is missing or outside the project."
            );
            if (id == "location")
            {
                Require(
                    relative == "docker-compose.yml"
                        && File.ReadAllText(config)
                            .Contains("redis:7.2-alpine", StringComparison.Ordinal),
                    "Location must reference the Redis compose service."
                );
                continue;
            }
            var loaded =
                JsonSerializer.Deserialize<NodeSettings>(File.ReadAllText(config))
                ?? throw new InvalidDataException("Invalid node configuration.");
            Require(loaded.Id == id, "Configuration ID differs from structure ID.");
            Require(
                (id == "session") == (loaded.StreamEndpoint is not null),
                "STREAM listener belongs to Session only."
            );
            settings.Add(id, loaded);
        }
        var zonesA =
            settings["zoneA"].ZoneIds
            ?? throw new InvalidDataException("Zone node A has no configured zones.");
        var zonesB =
            settings["zoneB"].ZoneIds
            ?? throw new InvalidDataException("Zone node B has no configured zones.");
        Require(
            zonesA.SequenceEqual(new[] { "zone-1", "zone-2", "zone-3" })
                && zonesB.SequenceEqual(new[] { "zone-4", "zone-5", "zone-6" }),
            "Configured zones differ from topology."
        );
        Require(
            settings["zoneA"].ZoneCapacity == zonesA.Length
                && settings["zoneB"].ZoneCapacity == zonesA.Length + zonesB.Length,
            "Zone capacity must support initial placement and host drain."
        );
        var channels = Array(document, "channels");
        Require(channels.Count == 2, "Both zone nodes must own border membership.");
        Require(
            channels
                .Select(channel => Text(channel, "owner"))
                .Order()
                .SequenceEqual(new[] { "zoneA", "zoneB" }),
            "Wrong border owners."
        );
        foreach (var channel in channels)
            Require(
                Text(channel, "name") == GameNames.BorderChannel
                    && Text(channel, "mesh") == GameNames.WorldMesh,
                "Channel differs from public registration names."
            );
        var spots = Array(document, "spots");
        Require(
            spots.Count == 9 && spots.Select(spot => Text(spot, "id")).Distinct().Count() == 9,
            "Two entries, six zones and one dynamic dungeon are required."
        );
        foreach (var spot in spots)
        {
            if (Text(spot, "kind") == "entry")
            {
                var id = Text(spot, "id");
                Require(
                    (id == "zone-entry-a" && Text(spot, "owner") == "zoneA")
                        || (id == "zone-entry-b" && Text(spot, "owner") == "zoneB"),
                    "Entry has the wrong zone owner."
                );
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry must not claim a stable type, logical ID or dynamic family."
                );
                continue;
            }
            Require(Text(spot, "kind") == "user", "Only User Spots belong to this preset.");
            var type = Text(spot, "type");
            var owner = Text(spot, "owner");
            if (type == GameNames.ZoneType)
                Require(
                    spot!["dynamic"]?.GetValue<bool>() != true
                        && settings.TryGetValue(owner, out var ownerSettings)
                        && ownerSettings.ZoneIds?.Contains(Text(spot, "logicalId")) == true,
                    "Zone Spot has the wrong configured owner or identity."
                );
            else
                Require(
                    type == GameNames.DungeonType
                        && Text(spot, "id") == "dungeon"
                        && owner == "instance"
                        && spot!["dynamic"]?.GetValue<bool>() == true
                        && spot["logicalId"] is null,
                    "Dungeon must be a dynamic Instance User Spot."
                );
        }
        Require(
            spots.Count(spot => Text(spot, "kind") == "entry") == 2
                && spots.Count(spot => spot?["type"]?.GetValue<string>() == GameNames.ZoneType) == 6
                && spots.Count(spot => spot?["type"]?.GetValue<string>() == GameNames.DungeonType)
                    == 1,
            "Spot category counts differ from runtime registration."
        );
        Require(
            spots
                .Where(spot => spot?["type"]?.GetValue<string>() == GameNames.ZoneType)
                .Select(spot => Text(spot, "logicalId"))
                .Order()
                .SequenceEqual(zonesA.Concat(zonesB).Order()),
            "User Spot logical IDs differ from configured zones."
        );
        var edges = Array(document, "edges");
        var expected = new[]
        {
            ("players", "session", "STREAM"),
            ("session", "zoneA", "relay"),
            ("zoneA", "zoneB", "relocation"),
            ("zoneB", "zoneA", "border pub/sub"),
            ("zoneB", "instance", "GetOrCreate"),
            ("zoneB", "location", "Location / lease"),
        };
        Require(
            edges.Count == expected.Length
                && edges
                    .Select(edge => (Text(edge, "from"), Text(edge, "to"), Text(edge, "label")))
                    .SequenceEqual(expected),
            "Edges differ from the seamless landing topology."
        );
        Require(
            Text(edges[3], "semantic") == "border-updates"
                && Text(edges[5], "semantic") == "location-authority",
            "Edge semantics are incorrect."
        );
    }
}
