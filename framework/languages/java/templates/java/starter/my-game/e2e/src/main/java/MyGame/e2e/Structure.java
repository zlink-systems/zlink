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
        var shells = new HashSet<String>();
        commands.fieldNames().forEachRemaining(shells::add);
        require(shells.equals(Set.of("sh", "powershell")), "Invalid command shells");
        for (var shell : shells)
            require(
                    !array(commands, shell).isEmpty()
                            && array(commands, shell).stream()
                                    .allMatch(v -> v.isTextual() && !v.asText().isBlank()),
                    "Invalid commands");
        var ids = new HashSet<String>();
        int sessions = 0;
        for (var node : array(s, "nodes")) {
            var id = text(node, "id");
            require(ids.add(id), "Duplicate node ID: " + id);
            var role = text(node, "role");
            text(node, "title");
            require(node.has("config"), "Missing config");
            if (role.equals(Roles.EXTERNAL)) {
                require(node.path("config").isNull(), "External config must be null");
                continue;
            }
            var file = configPath(root, text(node, "config"));
            if (role.equals(Roles.LOCATION)) continue;
            require(role.equals(Roles.SESSION), "No entry point is registered for role: " + role);
            var config = Json.MAPPER.readValue(file.toFile(), NodeConfig.class);
            require(config.id().equals(id), "Configuration Id mismatch: " + file);
            require(
                    config.streamEndpoint() != null && !config.streamEndpoint().isBlank(),
                    "Session requires streamEndpoint");
            sessions++;
        }
        require(sessions > 0, "At least one application node must be declared");
        require(array(s, "channels").isEmpty(), "No application channels are registered");
        require(array(s, "spots").isEmpty(), "No application Spots are registered");
        for (var edge : array(s, "edges")) {
            require(
                    ids.contains(text(edge, "from")) && ids.contains(text(edge, "to")),
                    "Unknown edge endpoint");
            text(edge, "label");
            if (edge.has("semantic")) text(edge, "semantic");
        }
        System.out.println("Structure: valid");
    }
}
