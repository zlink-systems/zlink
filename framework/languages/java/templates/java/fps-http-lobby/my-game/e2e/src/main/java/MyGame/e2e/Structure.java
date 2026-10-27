package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;

import java.nio.file.*;
import java.util.*;

public final class Structure {
    private Structure() {}

    private record Edge(String from, String to, String label, String semantic) {}

    private static final int FORMAT_VERSION = 1;

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
                s.path("version").isIntegralNumber()
                        && s.path("version").canConvertToInt()
                        && s.path("version").asInt() == FORMAT_VERSION,
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
        var scenarioArgument = String.join(",", Names.SCENARIOS);
        require(array(commands, "sh").stream().map(JsonNode::asText).toList().equals(List.of(
                "./run-e2e.sh --build-only", "./run-e2e.sh", "./run-e2e.sh --scenario " + scenarioArgument)),
                "Invalid shell commands");
        require(array(commands, "powershell").stream().map(JsonNode::asText).toList().equals(List.of(
                "pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly", "pwsh -NoProfile -File ./run-e2e.ps1",
                "pwsh -NoProfile -File ./run-e2e.ps1 -Scenario " + scenarioArgument)), "Invalid PowerShell commands");
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
            if (role.equals(Roles.EXTERNAL))
                require(node.path("config").isNull(), "External config must be null");
            else {
                var file = configPath(root, text(node, "config"));
                if (!role.equals(Roles.LOCATION)) {
                    var config = Json.MAPPER.readTree(file.toFile());
                    require(
                            config.path("id").asText().equals(id),
                            "Configuration Id mismatch: " + file);
                    if (role.equals(Roles.MATCH))
                        require(
                                config.path("roomCapacity").asInt() > 0,
                                "Room capacity must be positive");
                }
            }
        }
        var channels = array(s, "channels");
        unique(channels.stream().map(c -> List.of(text(c, "name"), owner(c, roles))).toList());
        require(channels.isEmpty(), "Server registers no channels");
        var spots = array(s, "spots");
        unique(spots.stream().map(p -> text(p, "id")).toList());
        var logical = new ArrayList<String>();
        boolean dynamic = false;
        for (var spot : spots) {
            require(
                    roles.get(owner(spot, roles)).equals(Roles.MATCH),
                    "Room Spot owner must be a match node");
            switch (text(spot, "kind")) {
                case "entry" ->
                        require(
                                spot.path("type").isNull()
                                        && !spot.has("logicalId")
                                        && !spot.has("dynamic"),
                                "Invalid Entry Spot declaration");
                case "user" -> {
                    require(text(spot, "type").equals(Names.ROOM), "Unregistered stable type");
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
        require(dynamic, "Missing dynamic Room family");
        for (var node : nodes)
            if (roles.get(text(node, "id")).equals(Roles.MATCH))
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
                        "Match node must declare exactly one application Entry Spot");
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
                        new Edge("players", "lobby", "HTTP", ""),
                        new Edge("players", "session", "STREAM", ""),
                        new Edge("session", "matchA", "relay", ""),
                        new Edge("lobby", "location", "Find room", ""),
                        new Edge("matchA", "location", "Location / lease", "location-authority")))
            require(declaredEdges.contains(required), "Missing required edge: " + required);
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
