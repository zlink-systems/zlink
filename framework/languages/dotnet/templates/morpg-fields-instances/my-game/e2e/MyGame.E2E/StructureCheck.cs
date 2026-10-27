using System.Text.Json.Nodes;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class StructureCheck
{
    private static readonly (
        string From,
        string To,
        string Label,
        string? Semantic
    )[] RequiredEdges =
    [
        ("players", "session", "STREAM", null),
        ("session", "fieldA", "relay", null),
        ("fieldA", "fieldB", "relocation", null),
        ("fieldB", "fieldA", "border pub/sub", "border-updates"),
        ("fieldB", "instanceA", "GetOrCreate", null),
        ("fieldB", "location", "Location / lease", "location-authority"),
    ];

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
        var registrations = new HashSet<(string Name, string Owner)>();
        foreach (var channel in Array(document, "channels"))
        {
            var owner = Text(channel, "owner");
            var name = Text(channel, "name");
            Require(
                roles.TryGetValue(owner, out var role) && role == "field",
                "Border channel owner must register the Field server."
            );
            Require(
                name == GameNames.BorderChannel && Text(channel, "mesh") == GameNames.WorldMesh,
                "Channel differs from server registration."
            );
            Require(registrations.Add((name, owner)), "Duplicate channel registration.");
        }
        Require(
            roles
                .Where(pair => pair.Value == "field")
                .All(pair => registrations.Contains((GameNames.BorderChannel, pair.Key))),
            "Missing Field border registration."
        );
        var spotIds = new HashSet<string>(StringComparer.Ordinal);
        var logicalIds = new HashSet<string>(StringComparer.Ordinal);
        var entries = new HashSet<string>(StringComparer.Ordinal);
        var userTypes = new HashSet<string>(StringComparer.Ordinal);
        var fields = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        foreach (var spot in Array(document, "spots"))
        {
            Require(spotIds.Add(Text(spot, "id")), "Duplicate Spot ID.");
            var owner = Text(spot, "owner");
            Require(settings.ContainsKey(owner), "Spot owner must reference an application node.");
            var kind = Text(spot, "kind");
            if (kind == "entry")
            {
                Require(
                    roles[owner] is "field" or "instance",
                    "Entry owner must register application Entry and its first-entry handler."
                );
                Require(entries.Add(owner), "Duplicate Entry owner.");
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry must not claim a stable type, logical ID or dynamic family."
                );
                continue;
            }
            var type = Text(spot, "type");
            Require(
                kind == "user" && (type == GameNames.FieldType || type == GameNames.PartyType || type == GameNames.DungeonType),
                "Spot kind or type differs from server registration."
            );
            Require(
                roles[owner] == (type != GameNames.DungeonType ? "field" : "instance"),
                "Spot type differs from its owner registration."
            );
            userTypes.Add(type);
            var dynamic = false;
            if (spot!["dynamic"] is JsonNode dynamicNode)
            {
                Require(
                    dynamicNode is JsonValue flag && flag.TryGetValue<bool>(out dynamic),
                    "Invalid dynamic flag."
                );
            }
            Require(
                dynamic == (type is GameNames.DungeonType or GameNames.PartyType),
                "Spot family dynamic flag differs from registration."
            );
            if (dynamic)
                Require(spot["logicalId"] is null, "Dynamic Spot family must omit logicalId.");
            else
                Require(logicalIds.Add(Text(spot, "logicalId")), "Duplicate logical Spot ID.");
            if (type == GameNames.FieldType)
            {
                if (!fields.TryGetValue(owner, out var hosted))
                    fields.Add(owner, hosted = new HashSet<string>(StringComparer.Ordinal));
                hosted.Add(Text(spot, "logicalId"));
            }
        }
        var entryOwners = roles
            .Where(pair => pair.Value is "field" or "instance")
            .Select(pair => pair.Key)
            .ToHashSet(StringComparer.Ordinal);
        Require(
            entries.SetEquals(entryOwners),
            "Entry owners differ from application Entry registrations."
        );
        Require(
            userTypes.SetEquals([GameNames.FieldType, GameNames.PartyType, GameNames.DungeonType]),
            "Missing registered User Spot type."
        );
        foreach (var (id, configured) in settings)
        {
            var expected = configured.FieldIds ?? System.Array.Empty<string>();
            if (roles[id] == "field")
                Require(
                    expected.Length > 0 && configured.FieldCapacity == expected.Length,
                    "Sequential Field bootstrap must fill the owner capacity."
                );
            Require(
                expected.All(field => !string.IsNullOrWhiteSpace(field))
                    && expected.Distinct(StringComparer.Ordinal).Count() == expected.Length,
                "Configured Field IDs must be nonempty and unique."
            );
            var declared = fields.TryGetValue(id, out var hosted) ? hosted : new HashSet<string>();
            Require(
                declared.SetEquals(expected),
                "Field logical IDs differ from owning configuration."
            );
        }
        var edges = Array(document, "edges");
        foreach (var required in RequiredEdges)
            Require(
                edges.Any(edge =>
                    Text(edge, "from") == required.From
                    && Text(edge, "to") == required.To
                    && Text(edge, "label") == required.Label
                    && edge?["semantic"]?.GetValue<string>() == required.Semantic
                ),
                $"Missing required edge: {required.From} -> {required.To} ({required.Label})."
            );
        foreach (var edge in edges)
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
