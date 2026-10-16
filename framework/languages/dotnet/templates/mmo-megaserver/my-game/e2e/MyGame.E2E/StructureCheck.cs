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

    public static void Run(string root, string? path)
    {
        var document =
            JsonNode.Parse(File.ReadAllText(path ?? Path.Combine(root, "zlink.structure.json")))
                as JsonObject
            ?? throw new InvalidDataException("Structure must be an object.");
        Validate(root, document);
        Console.WriteLine("✓ structure agrees with configs and typed contracts");
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
        Require(
            Text(document, "language") == "dotnet",
            "Structure language differs from this project."
        );
        ValidateRun(document);
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var roles = new Dictionary<string, string>(StringComparer.Ordinal);
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
            roles.Add(id, role);
        }
        Require(
            Array(document, "channels").Count == 0,
            "This template does not register application mesh channels."
        );
        var spotIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var spot in Array(document, "spots"))
        {
            Require(spotIds.Add(Text(spot, "id")), "Duplicate Spot ID.");
            var owner = Text(spot, "owner");
            Require(settings.ContainsKey(owner), "Spot owner must reference an application node.");
            var kind = Text(spot, "kind");
            if (kind == "entry")
            {
                Require(
                    roles[owner] == "region",
                    "Entry owner must register the application Entry handler."
                );
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry must not claim a stable type, logical ID or dynamic family."
                );
                continue;
            }
            var type = Text(spot, "type");
            Require(
                kind == "user" && (type == GameNames.RegionType || type == GameNames.DungeonType),
                "Spot kind or type differs from server registration."
            );
            Require(
                roles[owner] == (type == GameNames.RegionType ? "region" : "instance"),
                "Spot type differs from its owner registration."
            );
            var dynamic = false;
            if (spot!["dynamic"] is JsonNode dynamicNode)
            {
                Require(
                    dynamicNode is JsonValue flag && flag.TryGetValue<bool>(out dynamic),
                    "Invalid dynamic flag."
                );
            }
            Require(spot["logicalId"] is null, "Dynamic Spot family must omit logicalId.");
            Require(dynamic, "Region and Dungeon registrations are dynamic families.");
        }
        var spots = Array(document, "spots");
        foreach (var owner in roles.Where(pair => pair.Value == "region").Select(pair => pair.Key))
            Require(
                spots.Count(spot => Text(spot, "kind") == "entry" && Text(spot, "owner") == owner)
                    == 1,
                "Expected one application Entry Spot: " + owner
            );
        foreach (var type in new[] { GameNames.RegionType, GameNames.DungeonType })
            Require(
                spots.Any(spot => Text(spot, "kind") == "user" && Text(spot, "type") == type),
                "Missing registered User Spot type: " + type
            );
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
