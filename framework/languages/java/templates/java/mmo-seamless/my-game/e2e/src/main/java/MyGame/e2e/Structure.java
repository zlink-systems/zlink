package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;

import java.nio.file.*;
import java.util.*;

public final class Structure {
    private Structure() {}

    static void require(boolean value, String message) {
        if (!value) throw new IllegalStateException(message);
    }

    private static String text(JsonNode node, String key) {
        var value = node.get(key);
        require(
                value != null && value.isTextual() && !value.asText().isBlank(),
                "Missing string: " + key);
        return value.asText();
    }

    private static List<JsonNode> array(JsonNode node, String key) {
        var value = node.get(key);
        require(value != null && value.isArray(), "Missing array: " + key);
        var result = new ArrayList<JsonNode>();
        value.forEach(result::add);
        return result;
    }

    private static void unique(List<?> values) {
        require(new HashSet<>(values).size() == values.size(), "Duplicate identity: " + values);
    }

    private static Path configPath(Path root, String value) throws Exception {
        var path = Path.of(value);
        require(
                !path.isAbsolute() && !value.contains("\\") && !value.matches("^[A-Za-z]:.*"),
                "Invalid relative path: " + value);
        var resolved = root.resolve(path).normalize();
        require(
                resolved.startsWith(root)
                        && Files.isRegularFile(resolved)
                        && resolved.toRealPath().startsWith(root.toRealPath()),
                "Configuration escapes root: " + value);
        return resolved;
    }

    public static void check(Path root) throws Exception {
        var s = Json.MAPPER.readTree(root.resolve("zlink.structure.json").toFile());
        require(
                s.path("version").isIntegralNumber() && s.path("version").asInt() == 1,
                "Unsupported structure version");
        text(s, "id");
        text(s, "name");
        require(
                text(s, "language").equals(Names.LANGUAGE),
                "Structure language differs from server");
        var run = s.path("run");
        require(
                array(run, "scenarios").stream()
                        .map(JsonNode::asText)
                        .toList()
                        .equals(Names.SCENARIOS),
                "Scenario list differs from runner");
        var commands = run.path("commands");
        var keys = new HashSet<String>();
        commands.fieldNames().forEachRemaining(keys::add);
        require(keys.equals(Set.of("sh", "powershell")), "Invalid command shells");
        for (var shell : keys) {
            var values = array(commands, shell);
            require(
                    !values.isEmpty()
                            && values.stream()
                                    .allMatch(v -> v.isTextual() && !v.asText().isBlank()),
                    "Invalid commands");
        }
        var nodes = array(s, "nodes");
        var ids = nodes.stream().map(n -> text(n, "id")).toList();
        unique(ids);
        var roles = new HashMap<String, String>();
        var configs = new HashMap<String, NodeConfig>();
        for (var node : nodes) {
            var id = text(node, "id");
            var role = text(node, "role");
            text(node, "title");
            roles.put(id, role);
            require(node.has("config"), "Missing config");
            if (role.equals(Roles.EXTERNAL))
                require(node.path("config").isNull(), "External config must be null");
            else {
                var file = configPath(root, text(node, "config"));
                if (!role.equals(Roles.LOCATION)) {
                    var config = Json.MAPPER.readValue(file.toFile(), NodeConfig.class);
                    require(config.id().equals(id), "Configuration Id mismatch: " + file);
                    configs.put(id, config);
                }
            }
        }
        var channels = array(s, "channels");
        unique(channels.stream().map(c -> List.of(text(c, "name"), owner(c, roles))).toList());
        for (var channel : channels)
            require(
                    text(channel, "name").equals(Names.BORDER)
                            && text(channel, "mesh").equals(Names.MESH)
                            && roles.get(owner(channel, roles)).equals(Roles.ZONE),
                    "Unregistered border channel");
        var spots = array(s, "spots");
        unique(spots.stream().map(p -> text(p, "id")).toList());
        var logical = new ArrayList<String>();
        boolean dynamic = false;
        for (var spot : spots) {
            require(
                    Set.of(Roles.ZONE, Roles.INSTANCE).contains(roles.get(owner(spot, roles))),
                    "Spot owner must be a Zone or Instance node");
            switch (text(spot, "kind")) {
                case "entry" ->
                        require(
                                roles.get(owner(spot, roles)).equals(Roles.ZONE)
                                        && spot.path("type").isNull()
                                        && !spot.has("logicalId")
                                        && !spot.has("dynamic"),
                                "Invalid Entry Spot declaration");
                case "user" -> {
                    require(
                            text(spot, "type")
                                    .equals(
                                            roles.get(owner(spot, roles)).equals(Roles.ZONE)
                                                    ? Names.ZONE
                                                    : Names.DUNGEON),
                            "Unregistered stable type");
                    if (spot.has("dynamic"))
                        require(spot.path("dynamic").isBoolean(), "Invalid dynamic flag");
                    if (spot.path("dynamic").asBoolean()) {
                        require(!spot.has("logicalId"), "Dynamic Spot declares logicalId");
                        require(
                                text(spot, "type").equals(Names.DUNGEON),
                                "Only Dungeon is dynamic");
                        dynamic = true;
                    } else logical.add(text(spot, "logicalId"));
                }
                default -> throw new IllegalStateException("Unregistered Spot kind");
            }
        }
        unique(logical);
        require(dynamic, "Missing dynamic Dungeon family");
        for (var node : nodes)
            if (roles.get(text(node, "id")).equals(Roles.ZONE)) {
                var id = text(node, "id");
                var configured = configs.get(id).zoneIds();
                var declared =
                        spots.stream()
                                .filter(
                                        spot ->
                                                spot.path("owner").asText().equals(id)
                                                        && spot.path("type")
                                                                .asText()
                                                                .equals(Names.ZONE)
                                                        && !spot.path("dynamic").asBoolean())
                                .map(spot -> text(spot, "logicalId"))
                                .toList();
                unique(configured);
                require(
                        new HashSet<>(configured).equals(new HashSet<>(declared)),
                        "Configured ZoneIds differ from fixed Spots: " + id);
                require(
                        channels.stream().anyMatch(c -> c.path("owner").asText().equals(id)),
                        "Missing border channel: " + id);
                require(
                        spots.stream()
                                .anyMatch(
                                        p ->
                                                p.path("owner").asText().equals(text(node, "id"))
                                                        && p.path("kind").asText().equals("entry")),
                        "Zone node lacks Entry Spot");
            }
        for (var edge : array(s, "edges")) {
            require(
                    ids.contains(text(edge, "from")) && ids.contains(text(edge, "to")),
                    "Unknown edge endpoint");
            text(edge, "label");
            if (edge.has("semantic")) text(edge, "semantic");
        }
        System.out.println("Structure: valid");
    }

    private static String owner(JsonNode node, Map<String, String> roles) {
        var id = text(node, "owner");
        require(
                roles.containsKey(id)
                        && !Set.of(Roles.EXTERNAL, Roles.LOCATION).contains(roles.get(id)),
                "Invalid owner: " + id);
        return id;
    }
}
