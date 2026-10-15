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
        var roles = new HashMap<String, NodeRole>();
        for (var node : nodes) {
            var id = text(node, "id");
            var role = NodeRole.parse(text(node, "role"));
            text(node, "title");
            roles.put(id, role);
            require(node.has("config"), "Missing config");
            if (role == NodeRole.EXTERNAL)
                require(node.path("config").isNull(), "External config must be null");
            else {
                var file = configPath(root, text(node, "config"));
                if (role != NodeRole.LOCATION_STORE)
                    require(
                            Json.MAPPER.readTree(file.toFile()).path("id").asText().equals(id),
                            "Configuration Id mismatch: " + file);
            }
        }
        for (var node : nodes)
            if (NodeRole.parse(node.path("role").asText()) == NodeRole.INSTANCE) {
                var config = Json.MAPPER.readTree(configPath(root, text(node, "config")).toFile());
                require(
                        config.path("dungeonCapacity").isIntegralNumber()
                                && config.path("dungeonCapacity").asInt() > 0,
                        "Invalid dungeon capacity");
            }
        var channels = array(s, "channels");
        unique(channels.stream().map(c -> List.of(text(c, "name"), owner(c, roles))).toList());
        require(channels.isEmpty(), "Server registers no channels");
        var spots = array(s, "spots");
        unique(spots.stream().map(p -> text(p, "id")).toList());
        var logical = new ArrayList<String>();
        var dynamic = new HashSet<String>();
        for (var spot : spots) {
            NodeRole role = roles.get(owner(spot, roles));
            switch (text(spot, "kind")) {
                case "entry" ->
                        require(
                                role == NodeRole.TOWN
                                        && spot.has("type")
                                        && spot.path("type").isNull()
                                        && !spot.has("logicalId")
                                        && !spot.has("dynamic"),
                                "Invalid Entry Spot declaration");
                case "user" -> {
                    var type = text(spot, "type");
                    require(
                            (type.equals(Names.TOWN) && role == NodeRole.TOWN)
                                    || (type.equals(Names.DUNGEON) && role == NodeRole.INSTANCE),
                            "Unregistered stable type or owner");
                    if (spot.has("dynamic"))
                        require(spot.path("dynamic").isBoolean(), "Invalid dynamic flag");
                    if (spot.path("dynamic").asBoolean()) {
                        require(
                                type.equals(Names.DUNGEON) && !spot.has("logicalId"),
                                "Invalid dynamic Dungeon declaration");
                        dynamic.add(type);
                    } else {
                        var logicalId = text(spot, "logicalId");
                        logical.add(logicalId);
                        require(type.equals(Names.TOWN), "Only Town has fixed logical IDs");
                    }
                }
                default -> throw new IllegalStateException("Unregistered Spot kind");
            }
        }
        unique(logical);
        for (var node : nodes) {
            if (roles.get(text(node, "id")) != NodeRole.TOWN) continue;
            var config = Json.MAPPER.readTree(configPath(root, text(node, "config")).toFile());
            require(
                    config.path("townCapacity").isIntegralNumber()
                            && config.path("townCapacity").asInt() > 0,
                    "Invalid town capacity");
            var configured = array(config, "townIds").stream().map(JsonNode::asText).toList();
            unique(configured);
            var declared =
                    spots.stream()
                            .filter(
                                    spot ->
                                            spot.path("owner").asText().equals(text(node, "id"))
                                                    && spot.path("type")
                                                            .asText()
                                                            .equals(Names.TOWN))
                            .map(spot -> text(spot, "logicalId"))
                            .toList();
            require(
                    new HashSet<>(configured).equals(new HashSet<>(declared)),
                    "Fixed Town set differs from configuration");
        }
        require(dynamic.equals(Set.of(Names.DUNGEON)), "Missing dynamic Spot family");
        for (var node : nodes)
            if (roles.get(text(node, "id")) == NodeRole.TOWN)
                require(
                        spots.stream()
                                .anyMatch(
                                        p ->
                                                p.path("owner").asText().equals(text(node, "id"))
                                                        && p.path("kind").asText().equals("entry")),
                        "Town node lacks Entry Spot");
        for (var edge : array(s, "edges")) {
            require(
                    ids.contains(text(edge, "from")) && ids.contains(text(edge, "to")),
                    "Unknown edge endpoint");
            text(edge, "label");
            if (edge.has("semantic")) text(edge, "semantic");
        }
        System.out.println("Structure: valid");
    }

    private static String owner(JsonNode node, Map<String, NodeRole> roles) {
        var id = text(node, "owner");
        require(roles.containsKey(id) && roles.get(id).application(), "Invalid owner: " + id);
        return id;
    }
}
