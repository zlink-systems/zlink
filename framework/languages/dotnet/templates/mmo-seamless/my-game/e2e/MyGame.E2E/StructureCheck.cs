using System.Text.Json.Nodes;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class StructureCheck
{
    private static readonly string[] CommandShells = ["sh", "powershell"];

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidDataException(message);
    }

    private static string Text(JsonNode? node, string name)
    {
        if (
            node?[name] is JsonValue value
            && value.TryGetValue<string>(out var text)
            && !string.IsNullOrWhiteSpace(text)
        )
            return text;
        throw new InvalidDataException($"Missing or invalid {name}.");
    }

    private static JsonObject Object(JsonNode? node, string name) =>
        node as JsonObject ?? throw new InvalidDataException($"Missing or invalid {name} object.");

    private static string[] Strings(JsonNode? node, string name)
    {
        if (node is not JsonArray values || values.Count == 0)
            throw new InvalidDataException($"Missing or invalid {name} array.");
        var strings = new string[values.Count];
        for (var index = 0; index < values.Count; index++)
        {
            if (
                values[index] is not JsonValue value
                || !value.TryGetValue<string>(out var text)
                || string.IsNullOrWhiteSpace(text)
            )
                throw new InvalidDataException($"Missing or invalid {name} string.");
            strings[index] = text;
        }
        return strings;
    }

    private static void ValidateRun(JsonObject document)
    {
        var run = Object(document["run"], "run");
        var commands = Object(run["commands"], "run.commands");
        Require(
            commands.Count == CommandShells.Length && CommandShells.All(commands.ContainsKey),
            "run.commands must define sh and powershell."
        );
        foreach (var shell in CommandShells)
            _ = Strings(commands[shell], $"run.commands.{shell}");
        Require(
            Strings(run["scenarios"], "run.scenarios").SequenceEqual(Scenarios.All),
            "run.scenarios differs from the e2e scenario list."
        );
    }

    private static JsonArray Array(JsonObject document, string name)
    {
        var values =
            document[name] as JsonArray ?? throw new InvalidDataException($"Missing {name} array.");
        Require(values.All(value => value is JsonObject), $"Each {name} item must be an object.");
        return values;
    }

    public static void Run(string root, string? path, bool selfTest = false)
    {
        var document =
            JsonNode.Parse(File.ReadAllText(path ?? Path.Combine(root, "zlink.structure.json")))
                as JsonObject
            ?? throw new InvalidDataException("Structure must be an object.");
        Validate(root, document);
        Console.WriteLine("✓ structure agrees with configs and typed contracts");
        if (!selfTest)
            return;

        var expanded = (JsonObject)document.DeepClone();
        var ids = Array(expanded, "nodes").Select(node => Text(node, "id")).ToHashSet();
        ids.UnionWith(Array(expanded, "spots").Select(spot => Text(spot, "id")));
        var addedId = "structure-check-external";
        while (ids.Contains(addedId))
            addedId += "-extra";
        Array(expanded, "nodes")
            .Add(
                new JsonObject
                {
                    ["id"] = addedId,
                    ["role"] = "external",
                    ["title"] = "Additional client",
                    ["config"] = null,
                }
            );
        var application = Array(document, "nodes")
            .FirstOrDefault(node =>
                Text(node, "role") != "external" && Text(node, "role") != "location-store"
            );
        JsonArray RunScenarios(JsonObject value) => value["run"]!["scenarios"]!.AsArray();
        JsonArray RunCommands(JsonObject value, string shell) =>
            value["run"]!["commands"]![shell]!.AsArray();
        if (application is not null)
        {
            var spotIds = Array(expanded, "spots").Select(spot => Text(spot, "id")).ToHashSet();
            var addedSpotId = "structure-check-entry";
            while (spotIds.Contains(addedSpotId))
                addedSpotId += "-extra";
            Array(expanded, "spots")
                .Add(
                    new JsonObject
                    {
                        ["id"] = addedSpotId,
                        ["kind"] = "entry",
                        ["type"] = null,
                        ["owner"] = Text(application, "id"),
                    }
                );
        }
        Array(expanded, "edges")
            .Add(
                new JsonObject
                {
                    ["from"] = addedId,
                    ["to"] = application is null ? addedId : Text(application, "id"),
                    ["label"] = "Additional flow",
                }
            );
        foreach (var name in new[] { "nodes", "channels", "spots", "edges" })
        {
            var values = Array(expanded, name);
            expanded[name] = new JsonArray(
                values.Reverse().Select(value => value!.DeepClone()).ToArray()
            );
        }
        expanded["id"] = "custom-composition";
        Validate(root, expanded);
        Console.WriteLine("✓ structure accepts composition extensions and reordered arrays");

        JsonNode ConfiguredNode(JsonObject value) =>
            Array(value, "nodes")
                .First(node =>
                    node?["config"] is not null && Text(node, "role") != "location-store"
                )!;
        JsonNode Zone(JsonObject value) =>
            Array(value, "spots")
                .First(spot => spot?["type"]?.GetValue<string>() == GameNames.ZoneType)!;
        var mutations = new List<(string Name, Action<JsonObject> Change)>
        {
            ("unknown version", value => value["version"] = 2),
            ("empty project ID", value => value["id"] = " "),
            (
                "missing run scenario",
                value => RunScenarios(value).RemoveAt(RunScenarios(value).Count - 1)
            ),
            ("empty run command list", value => RunCommands(value, "sh").Clear()),
        };
        if (Array(document, "nodes").Count > 0)
            mutations.Add(
                (
                    "duplicate node",
                    value => Array(value, "nodes").Add(Array(value, "nodes").First()!.DeepClone())
                )
            );
        if (application is not null)
        {
            mutations.AddRange(
                new (string Name, Action<JsonObject> Change)[]
                {
                    (
                        "missing config",
                        value => ConfiguredNode(value)["config"] = "server/config/missing.json"
                    ),
                    (
                        "outside config",
                        value => ConfiguredNode(value)["config"] = "../outside.json"
                    ),
                    (
                        "rooted config",
                        value => ConfiguredNode(value)["config"] = Path.GetFullPath(root)
                    ),
                    ("configuration ID mismatch", value => ConfiguredNode(value)["id"] = addedId),
                }
            );
        }
        if (Array(document, "spots").Count > 0)
            mutations.Add(
                (
                    "duplicate spot",
                    value => Array(value, "spots").Add(Array(value, "spots").First()!.DeepClone())
                )
            );
        if (
            Array(document, "spots")
                .Any(spot => spot?["type"]?.GetValue<string>() == GameNames.ZoneType)
        )
        {
            mutations.AddRange(
                new (string Name, Action<JsonObject> Change)[]
                {
                    ("unknown spot owner", value => Zone(value)["owner"] = addedId),
                    (
                        "wrong logical ID",
                        value => Zone(value)["logicalId"] = "structure-check-missing-zone"
                    ),
                    ("missing configured zone", value => Array(value, "spots").Remove(Zone(value))),
                    (
                        "wrong spot type",
                        value => Zone(value)["type"] = "structure-check-unknown-type"
                    ),
                    (
                        "duplicate logical ID across types",
                        value =>
                        {
                            var spot = (JsonObject)Zone(value).DeepClone();
                            spot["id"] = addedId;
                            spot["type"] = GameNames.DungeonType;
                            spot["dynamic"] = false;
                            Array(value, "spots").Add(spot);
                        }
                    ),
                }
            );
        }
        if (Array(document, "spots").Any(spot => Text(spot, "kind") == "entry"))
            mutations.Add(
                (
                    "entry stable type",
                    value =>
                        Array(value, "spots").First(spot => Text(spot, "kind") == "entry")![
                            "type"
                        ] = GameNames.ZoneType
                )
            );
        if (Array(document, "channels").Count > 0)
        {
            mutations.AddRange(
                new (string Name, Action<JsonObject> Change)[]
                {
                    (
                        "wrong channel",
                        value =>
                            Array(value, "channels").First()!["name"] =
                                "structure-check-unknown-channel"
                    ),
                    (
                        "wrong mesh",
                        value =>
                            Array(value, "channels").First()!["mesh"] =
                                "structure-check-unknown-mesh"
                    ),
                    (
                        "unknown channel owner",
                        value => Array(value, "channels").First()!["owner"] = addedId
                    ),
                    (
                        "duplicate channel",
                        value =>
                            Array(value, "channels")
                                .Add(Array(value, "channels").First()!.DeepClone())
                    ),
                }
            );
        }
        var external = Array(document, "nodes")
            .FirstOrDefault(node => Text(node, "role") == "external");
        if (external is not null && Array(document, "channels").Count > 0)
            mutations.Add(
                (
                    "external channel owner",
                    value => Array(value, "channels").First()!["owner"] = Text(external, "id")
                )
            );
        if (Array(document, "edges").Count > 0)
        {
            mutations.Add(
                ("unknown edge reference", value => Array(value, "edges").First()!["to"] = addedId)
            );
            mutations.Add(
                ("empty edge label", value => Array(value, "edges").First()!["label"] = " ")
            );
        }
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
        Console.WriteLine($"Structure: 2 valid inputs and {mutations.Count} invalid inputs passed");
    }

    private static void Validate(string root, JsonObject document)
    {
        Require(
            document["version"] is JsonValue version
                && version.TryGetValue<int>(out var number)
                && number == 1,
            "Unsupported structure version."
        );
        foreach (var field in new[] { "id", "name", "language" })
            Text(document, field);
        ValidateRun(document);
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var settings = new Dictionary<string, NodeSettings>(StringComparer.Ordinal);
        var prefix =
            Path.GetFullPath(root)
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        var pathComparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        foreach (var node in Array(document, "nodes"))
        {
            var id = Text(node, "id");
            Require(ids.Add(id), "Duplicate node ID.");
            var role = Text(node, "role");
            Text(node, "title");
            if (role == "external")
            {
                Require(
                    node!["config"] is null,
                    "External nodes must not have server configuration."
                );
                continue;
            }
            var relative = Text(node, "config");
            var config = Path.GetFullPath(Path.Combine(root, relative));
            Require(
                !Path.IsPathRooted(relative)
                    && !relative.Contains('\\')
                    && config.StartsWith(prefix, pathComparison)
                    && File.Exists(config),
                "Configuration is missing, not relative, or outside the project."
            );
            if (role == "location-store")
                continue;
            var loaded = NodeSettings.Load(new[] { "--config", config });
            Require(loaded.Id == id, "Configuration ID differs from structure ID.");
            settings.Add(id, loaded);
        }
        var channels = new HashSet<(string Name, string Owner)>();
        foreach (var channel in Array(document, "channels"))
        {
            var owner = Text(channel, "owner");
            var name = Text(channel, "name");
            Require(
                settings.ContainsKey(owner),
                "Channel owner must reference an application node."
            );
            Require(channels.Add((name, owner)), "Duplicate channel registration.");
            Require(
                name == GameNames.BorderChannel && Text(channel, "mesh") == GameNames.WorldMesh,
                "Channel differs from public registration names."
            );
        }
        var spotIds = new HashSet<string>(StringComparer.Ordinal);
        var logicalIds = new HashSet<string>(StringComparer.Ordinal);
        var zones = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        foreach (var spot in Array(document, "spots"))
        {
            Require(spotIds.Add(Text(spot, "id")), "Duplicate Spot ID.");
            var owner = Text(spot, "owner");
            Require(settings.ContainsKey(owner), "Spot owner must reference an application node.");
            var kind = Text(spot, "kind");
            if (kind == "entry")
            {
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry must not claim a stable type, logical ID or dynamic family."
                );
                continue;
            }
            var type = Text(spot, "type");
            Require(
                kind == "user" && (type == GameNames.ZoneType || type == GameNames.DungeonType),
                "Spot kind or type differs from server registration."
            );
            var dynamic = false;
            if (spot!["dynamic"] is JsonNode dynamicNode)
            {
                Require(
                    dynamicNode is JsonValue flag && flag.TryGetValue<bool>(out dynamic),
                    "Invalid dynamic flag."
                );
            }
            if (dynamic)
                Require(spot["logicalId"] is null, "Dynamic Spot family must omit logicalId.");
            else
                Require(logicalIds.Add(Text(spot, "logicalId")), "Duplicate logical Spot ID.");
            if (type == GameNames.ZoneType)
            {
                Require(!dynamic, "Configured Zone Spots must have fixed logical IDs.");
                if (!zones.TryGetValue(owner, out var hosted))
                    zones.Add(owner, hosted = new HashSet<string>(StringComparer.Ordinal));
                hosted.Add(Text(spot, "logicalId"));
            }
        }
        foreach (var (id, configured) in settings)
        {
            var expected = configured.ZoneIds ?? System.Array.Empty<string>();
            Require(
                expected.All(zone => !string.IsNullOrWhiteSpace(zone))
                    && expected.Distinct(StringComparer.Ordinal).Count() == expected.Length,
                "Configured Zone IDs must be nonempty and unique."
            );
            var declared = zones.TryGetValue(id, out var hosted) ? hosted : new HashSet<string>();
            Require(
                declared.SetEquals(expected),
                "Zone logical IDs differ from owning configuration."
            );
        }
        foreach (var edge in Array(document, "edges"))
        {
            Require(
                ids.Contains(Text(edge, "from")) && ids.Contains(Text(edge, "to")),
                "Unknown edge node."
            );
            Text(edge, "label");
            if (edge?["semantic"] is not null)
                Text(edge, "semantic");
        }
    }
}
