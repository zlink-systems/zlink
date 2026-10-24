using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
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
        ("players", "accounts", "HTTP", null),
        ("players", "sessionA", "STREAM", null),
        ("players", "sessionB", "STREAM", null),
        ("sessionA", "channelA1", "relay", null),
        ("sessionB", "channelB", "relay", null),
        ("channelA1", "channelA2", "relocation", null),
        ("channelA2", "dbA", "game data", null),
        ("channelB", "dbB", "game data", null),
    ];

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidDataException(message);
    }

    private static string Text(JsonNode? node, string name) =>
        node?[name] is JsonValue value
        && value.TryGetValue<string>(out var text)
        && !string.IsNullOrWhiteSpace(text)
            ? text
            : throw new InvalidDataException($"Missing or invalid {name}.");

    private static JsonObject Object(JsonNode? node, string name) =>
        node as JsonObject ?? throw new InvalidDataException($"Missing or invalid {name} object.");

    private static JsonArray Array(JsonObject document, string name)
    {
        var values =
            document[name] as JsonArray ?? throw new InvalidDataException($"Missing {name} array.");
        Require(values.All(value => value is JsonObject), $"Each {name} item must be an object.");
        return values;
    }

    private static string[] Strings(JsonNode? node, string name)
    {
        var values = node as JsonArray ?? throw new InvalidDataException($"Missing {name} array.");
        Require(values.Count > 0, $"Empty {name} array.");
        return values
            .Select(value =>
                value is JsonValue item
                && item.TryGetValue<string>(out var text)
                && !string.IsNullOrWhiteSpace(text)
                    ? text
                    : throw new InvalidDataException($"Invalid {name} string.")
            )
            .ToArray();
    }

    public static void Run(string root, string? path)
    {
        var document = Object(
            JsonNode.Parse(File.ReadAllText(path ?? Path.Combine(root, "zlink.structure.json"))),
            "structure"
        );
        Require(
            document["version"] is JsonValue version
                && version.TryGetValue<int>(out var number)
                && number == 1,
            "Unsupported structure version."
        );
        Text(document, "id");
        Text(document, "name");
        Require(Text(document, "language") == "dotnet", "Language differs from this template.");
        var run = Object(document["run"], "run");
        var commands = Object(run["commands"], "run.commands");
        Require(
            commands.Count == 2 && commands.ContainsKey("sh") && commands.ContainsKey("powershell"),
            "Commands must define sh and powershell."
        );
        foreach (var shell in new[] { "sh", "powershell" })
            _ = Strings(commands[shell], shell);
        Require(
            Strings(run["scenarios"], "scenarios").SequenceEqual(Scenarios.All),
            "Scenarios differ from the runner."
        );
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var roles = new Dictionary<string, string>(StringComparer.Ordinal);
        var settings = new Dictionary<string, NodeSettings>(StringComparer.Ordinal);
        var rootPath =
            Path.GetFullPath(root)
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        foreach (var node in Array(document, "nodes"))
        {
            var id = Text(node, "id");
            Require(ids.Add(id), "Duplicate node ID.");
            var role = Text(node, "role");
            roles.Add(id, role);
            Text(node, "title");
            if (role == "external")
            {
                Require(node!["config"] is null, "External node must omit configuration.");
                continue;
            }
            var relative = Text(node, "config");
            var config = Path.GetFullPath(Path.Combine(root, relative));
            Require(
                !Path.IsPathRooted(relative)
                    && !relative.Contains('\\')
                    && config.StartsWith(
                        rootPath,
                        OperatingSystem.IsWindows()
                            ? StringComparison.OrdinalIgnoreCase
                            : StringComparison.Ordinal
                    )
                    && File.Exists(config),
                "Configuration is missing or outside the template."
            );
            if (role == "world-db")
            {
                Require(
                    Regex.IsMatch(
                        File.ReadAllText(config),
                        @"(?m)^  " + Regex.Escape(id) + @":\s*$"
                    ),
                    "World DB is not a Compose service."
                );
                continue;
            }
            Require(
                role is "channel" or "session" or "accounts",
                "Unknown application registration role."
            );
            var loaded = NodeSettings.Load(["--config", config]);
            Require(loaded.Id == id, "Configuration ID differs from node ID.");
            Require(
                role != "session" || loaded.StreamEndpoint is not null,
                "Session is missing STREAM binding."
            );
            Require(
                role != "accounts" || loaded.HttpEndpoint is not null,
                "Accounts is missing HTTP binding."
            );
            settings.Add(id, loaded);
        }
        var worlds = settings
            .Where(pair => roles[pair.Key] != "accounts")
            .GroupBy(pair => pair.Value.WorldId)
            .ToArray();
        var namespaces = new HashSet<(string Redis, string Prefix)>();
        foreach (var world in worlds)
        {
            var first = world.First().Value;
            Require(
                namespaces.Add((first.RedisEndpoint, first.KeyPrefix)),
                "Worlds share a data namespace."
            );
            Require(
                world.All(pair =>
                    pair.Value.RedisEndpoint == first.RedisEndpoint
                    && pair.Value.KeyPrefix == first.KeyPrefix
                    && pair.Value.MeshName == first.MeshName
                ),
                "Nodes in one world disagree about storage or mesh."
            );
        }
        Require(
            worlds.Select(world => world.First().Value.RedisEndpoint).Distinct().Count()
                == worlds.Length,
            "Worlds must use separate Redis endpoints."
        );
        Require(
            worlds.Select(world => world.First().Value.MeshName).Distinct().Count()
                == worlds.Length,
            "Worlds must use separate meshes."
        );
        foreach (var account in settings.Where(pair => roles[pair.Key] == "accounts"))
        {
            var listed = account.Value.Worlds ?? [];
            Require(
                listed.Length == worlds.Length
                    && listed.Select(world => world.WorldId).Distinct().Count() == listed.Length
                    && listed.All(world =>
                        worlds.Any(group => group.Key == world.WorldId)
                        && !string.IsNullOrWhiteSpace(world.Name)
                        && settings.Any(pair =>
                            roles[pair.Key] == "session"
                            && pair.Value.WorldId == world.WorldId
                            && pair.Value.StreamEndpoint == world.SessionEndpoint
                        )
                    ),
                "Accounts world list differs from Session configuration."
            );
        }
        Require(
            Array(document, "channels").Count == 0,
            "No application mesh channels are registered."
        );
        var entries = new Dictionary<string, int>();
        var spotIds = new HashSet<string>();
        var logicalIds = new HashSet<(string Mesh, string Id)>();
        var zones = new Dictionary<string, HashSet<string>>();
        foreach (var spot in Array(document, "spots"))
        {
            Require(spotIds.Add(Text(spot, "id")), "Duplicate composition Spot ID.");
            var owner = Text(spot, "owner");
            Require(
                settings.ContainsKey(owner) && roles[owner] == "channel",
                "Spot owner does not register the Zone server."
            );
            var kind = Text(spot, "kind");
            if (kind == "entry")
            {
                Require(
                    spot!["type"] is null && spot["logicalId"] is null && spot["dynamic"] is null,
                    "Entry cannot claim stable type or identity."
                );
                entries[owner] = entries.GetValueOrDefault(owner) + 1;
                continue;
            }
            Require(
                kind == "user" && Text(spot, "type") == GameNames.ZoneType,
                "Spot registration differs from Zone factory."
            );
            Require(
                spot!["dynamic"] is null
                    || spot["dynamic"] is JsonValue flag
                        && flag.TryGetValue<bool>(out var dynamic)
                        && !dynamic,
                "Zones must have fixed identities."
            );
            var mesh = Text(spot, "mesh");
            Require(mesh == settings[owner].MeshName, "Zone mesh differs from owner registration.");
            var logical = Text(spot, "logicalId");
            Require(
                logical == GameNames.ZoneId(GameNames.Channel(logical)),
                "Invalid Zone logical ID."
            );
            Require(
                logicalIds.Add((mesh, logical)),
                "Duplicate logical Spot identity in one mesh."
            );
            if (!zones.TryGetValue(owner, out var declared))
                zones.Add(owner, declared = []);
            declared.Add(logical);
        }
        foreach (var (id, config) in settings)
        {
            Require(
                roles[id] != "channel" || entries.GetValueOrDefault(id) == 1,
                "Channel node requires one Entry Spot."
            );
            var configured = config.ZoneIds ?? [];
            Require(
                configured.Distinct().Count() == configured.Length
                    && configured.All(zone => !string.IsNullOrWhiteSpace(zone)),
                "Invalid configured Zone IDs."
            );
            Require(
                (zones.GetValueOrDefault(id) ?? []).SetEquals(configured),
                "Declared Zones differ from server configuration."
            );
            Require(
                roles[id] != "channel" || config.ZoneCapacity >= configured.Length,
                "Zone capacity is below bootstrap count."
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
                "Unknown edge endpoint."
            );
            Text(edge, "label");
            if (edge!["semantic"] is not null)
                Text(edge, "semantic");
        }
        Console.WriteLine("✓ structure agrees with configs and typed contracts");
    }
}
