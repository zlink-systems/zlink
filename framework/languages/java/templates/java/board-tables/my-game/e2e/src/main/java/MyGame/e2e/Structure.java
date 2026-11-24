package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;

import java.nio.file.*;
import java.util.*;

public final class Structure {
    private Structure() {}

    private record Channel(String name, String owner, String kind, String role) {}

    private record Edge(String from, String to, String label, String semantic) {}

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
        for (var node : nodes) {
            var id = text(node, "id");
            var role = text(node, "role");
            text(node, "title");
            roles.put(id, role);
            require(node.has("config"), "Missing config");
            if (role.equals("external"))
                require(node.path("config").isNull(), "External config must be null");
            else {
                var file = configPath(root, text(node, "config"));
                if (!role.equals("location-store")) {
                    var config = Json.MAPPER.readTree(file.toFile());
                    require(
                            config.path("id").asText().equals(id),
                            "Configuration Id mismatch: " + file);
                    if (role.equals("api")) text(config, "apiEndpoint");
                }
            }
        }
        require("api".equals(roles.get("api")), "Missing API node");
        require(
                roles.values().stream().filter("api"::equals).count() == 1,
                "Expected one API node");
        var channels =
                array(s, "channels").stream()
                        .map(
                                c ->
                                        new Channel(
                                                text(c, "name"),
                                                owner(c, roles),
                                                text(c, "kind"),
                                                text(c, "role")))
                        .toList();
        unique(channels.stream().map(c -> List.of(c.name(), c.owner())).toList());
        var expectedChannels = new HashSet<Channel>();
        expectedChannels.add(new Channel(Names.API, "api", "client-server", "server"));
        for (var entry : roles.entrySet()) {
            if (entry.getValue().equals("table"))
                expectedChannels.add(
                        new Channel(Names.API, entry.getKey(), "client-server", "client"));
        }
        require(Set.copyOf(channels).equals(expectedChannels), "Invalid API channel declarations");
        var spots = array(s, "spots");
        unique(spots.stream().map(p -> text(p, "id")).toList());
        var logical = new ArrayList<String>();
        boolean dynamic = false;
        int pools = 0;
        for (var spot : spots) {
            String role = roles.get(owner(spot, roles));
            if (spot.path("type").asText().equals(Names.POOL)) {
                require(
                        role.equals("matchmaker")
                                && text(spot, "kind").equals("user")
                                && text(spot, "logicalId").equals(Names.POOL_KEY)
                                && !spot.has("dynamic"),
                        "Invalid Pool spot declaration");
                logical.add(Names.POOL_KEY);
                pools++;
                continue;
            }
            require(role.equals("table"), "Table Spot owner must be a table node");
            switch (text(spot, "kind")) {
                case "entry" ->
                        require(
                                spot.path("type").isNull()
                                        && !spot.has("logicalId")
                                        && !spot.has("dynamic"),
                                "Invalid Entry Spot declaration");
                case "user" -> {
                    require(text(spot, "type").equals(Names.TABLE), "Unregistered stable type");
                    if (spot.has("dynamic"))
                        require(spot.path("dynamic").isBoolean(), "Invalid dynamic flag");
                    if (spot.path("dynamic").asBoolean()) {
                        require(!spot.has("logicalId"), "Dynamic Spot declares logicalId");
                        dynamic = true;
                    } else logical.add(text(spot, "logicalId"));
                }
                default -> throw new IllegalStateException("Unregistered Spot kind");
            }
        }
        unique(logical);
        require(pools == 1, "Matchmaker must declare exactly one Pool Spot");
        require(dynamic, "Missing dynamic Table family");
        for (var node : nodes)
            if (roles.get(text(node, "id")).equals("table"))
                require(
                        spots.stream()
                                        .filter(
                                                p ->
                                                        p.path("owner")
                                                                        .asText()
                                                                        .equals(text(node, "id"))
                                                                && p.path("kind")
                                                                        .asText()
                                                                        .equals("entry"))
                                        .count()
                                == 1,
                        "Table node must declare exactly one application Entry Spot");
        var declaredEdges = new HashSet<Edge>();
        for (var edge : array(s, "edges")) {
            require(
                    ids.contains(text(edge, "from")) && ids.contains(text(edge, "to")),
                    "Unknown edge endpoint");
            declaredEdges.add(
                    new Edge(
                            text(edge, "from"),
                            text(edge, "to"),
                            text(edge, "label"),
                            edge.has("semantic") ? text(edge, "semantic") : ""));
        }
        for (var required :
                Set.of(
                        new Edge("players", "session", "STREAM", ""),
                        new Edge("session", "playA", "relay", ""),
                        new Edge("playA", "matchmaker", "match requests", ""),
                        new Edge("playA", "api", "loadPlayer / commitRound", ""),
                        new Edge("playB", "api", "loadPlayer / commitRound", ""),
                        new Edge("api", "location", "Player records / round results", ""),
                        new Edge("matchmaker", "playA", "room preparation", ""),
                        new Edge("playA", "location", "Location / lease", "location-authority")))
            require(declaredEdges.contains(required), "Missing required edge: " + required);
        System.out.println("Structure: valid");
    }

    private static String owner(JsonNode node, Map<String, String> roles) {
        var id = text(node, "owner");
        require(
                roles.containsKey(id)
                        && !Set.of("external", "location-store").contains(roles.get(id)),
                "Invalid owner: " + id);
        return id;
    }
}
