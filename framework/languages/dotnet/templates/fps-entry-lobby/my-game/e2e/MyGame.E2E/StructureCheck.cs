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
            "Structure language differs from this server."
        );
        ValidateRun(document);
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
            var role = Text(node, "role");
            Require(roles.TryAdd(id, role), "Duplicate node ID.");
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
        Require(Array(document, "channels").Count == 0, "This server registers no channels.");
        var spotIds = new HashSet<string>(StringComparer.Ordinal);
        var logicalIds = new HashSet<string>(StringComparer.Ordinal);

        var entryOwners = new HashSet<string>(StringComparer.Ordinal);
        var matchmakingOwners = new HashSet<string>(StringComparer.Ordinal);
        var hasRoom = false;
        foreach (var spot in Array(document, "spots"))
        {
            Require(spotIds.Add(Text(spot, "id")), "Duplicate Spot ID.");
            var owner = Text(spot, "owner");
            Require(
                settings.ContainsKey(owner) && roles[owner] == "match",
                "Spot owner must reference a match node."
            );
            if (spot!["mesh"] is not null)
                Require(Text(spot, "mesh") == GameNames.WorldMesh, "Spot mesh differs from server registration.");
            var kind = Text(spot, "kind");
            if (kind == "entry")
            {
                Require(entryOwners.Add(owner), "Duplicate Entry owner.");
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry must not claim a stable type, logical ID or dynamic family."
                );
                continue;
            }
            var type = Text(spot, "type");
            Require(
                kind == "user" && (type == GameNames.RoomType || type == GameNames.MatchmakingType),
                "Spot kind or type differs from server registration."
            );
            if (type == GameNames.RoomType)
            {
                Require(spot!["dynamic"]?.GetValue<bool>() == true, "Room must be dynamic.");
                hasRoom = true;
            }
            if (type == GameNames.MatchmakingType)
            {
                matchmakingOwners.Add(owner);
                Require(
                    spot!["dynamic"] is null
                        && Text(spot, "logicalId") == GameNames.MatchmakingId
                        && settings[owner].BootstrapMatchmaking,
                    "Matchmaking must declare its bootstrap owner and fixed logical ID."
                );
            }
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
        }
        var matchOwners = roles
            .Where(pair => pair.Value == "match")
            .Select(pair => pair.Key)
            .ToHashSet(StringComparer.Ordinal);
        Require(
            matchOwners.Count > 0 && entryOwners.SetEquals(matchOwners),
            "Match nodes must each declare one Entry."
        );
        Require(hasRoom, "Missing dynamic Room family.");
        Require(
            matchmakingOwners.Count > 0
                && matchmakingOwners.SetEquals(
                    settings
                        .Where(pair =>
                            roles[pair.Key] == "match" && pair.Value.BootstrapMatchmaking
                        )
                        .Select(pair => pair.Key)
                ),
            "Bootstrap differs from fixed Matchmaking owner."
        );
        var edges = Array(document, "edges");
        foreach (var edge in edges)
        {
            Require(
                roles.ContainsKey(Text(edge, "from")) && roles.ContainsKey(Text(edge, "to")),
                "Unknown edge node."
            );
            Text(edge, "label");
            if (edge?["semantic"] is not null)
                Text(edge, "semantic");
        }
    }
}
