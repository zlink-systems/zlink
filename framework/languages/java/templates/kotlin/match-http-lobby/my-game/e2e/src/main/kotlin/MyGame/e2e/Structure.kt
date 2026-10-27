package MyGame.e2e

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import java.nio.file.Files
import java.nio.file.Path

private const val FORMAT_VERSION = 1

private fun text(node: JsonNode, key: String): String {
    val value = node[key]
    check(value != null && value.isTextual && value.asText().isNotBlank()) {
        "Missing string: $key"
    }
    return value.asText()
}

private fun array(node: JsonNode, key: String): List<JsonNode> {
    val value = node[key]
    check(value != null && value.isArray) { "Missing array: $key" }
    return value.toList()
}

private fun unique(values: List<*>) {
    check(values.distinct().size == values.size) { "Duplicate identity: $values" }
}

private fun configPath(root: Path, value: String): Path {
    val path = Path.of(value)
    check(!path.isAbsolute && !value.contains('\\') && !Regex("^[A-Za-z]:.*").matches(value)) {
        "Invalid relative path: $value"
    }
    val resolved = root.resolve(path).normalize()
    check(
        resolved.startsWith(root) &&
            Files.isRegularFile(resolved) &&
            resolved.toRealPath().startsWith(root.toRealPath())
    ) {
        "Configuration escapes root: $value"
    }
    return resolved
}

fun checkStructure(root: Path) {
    val s = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
    check(
        s.path("version").isIntegralNumber &&
            s.path("version").canConvertToInt() &&
            s.path("version").asInt() == FORMAT_VERSION
    ) {
        "Unsupported structure version"
    }
    text(s, "id")
    text(s, "name")
    check(text(s, "language") == Names.LANGUAGE) { "Structure language differs from server" }
    val run = s.path("run")
    check(array(run, "scenarios").map { it.asText() } == Names.SCENARIOS) {
        "Scenario list differs from runner"
    }
    val commands = run.path("commands")
    check(commands.fieldNames().asSequence().toSet() == setOf("sh", "powershell")) {
        "Invalid command shells"
    }
    for (shell in listOf("sh", "powershell")) {
        val values = array(commands, shell)
        check(values.isNotEmpty() && values.all { it.isTextual && it.asText().isNotBlank() }) {
            "Invalid commands"
        }
    }
    val nodes = array(s, "nodes")
    val ids = nodes.map { text(it, "id") }
    unique(ids)
    val roles = nodes.associate { text(it, "id") to text(it, "role") }
    for (node in nodes) {
        val id = text(node, "id")
        val role = text(node, "role")
        text(node, "title")
        check(node.has("config")) { "Missing config" }
        if (role == Roles.EXTERNAL)
            check(node.path("config").isNull) { "External config must be null" }
        else {
            val file = configPath(root, text(node, "config"))
            if (role != Roles.LOCATION) {
                val config = Json.mapper.readTree(file.toFile())
                check(config.path("id").asText() == id) { "Configuration Id mismatch: $file" }
                if (role == Roles.MATCH) {
                    check(config.path("roomCapacity").asInt() > 0) {
                        "Room capacity must be positive"
                    }
                }
            }
        }
    }
    fun owner(node: JsonNode): String {
        val id = text(node, "owner")
        check(id in roles && roles[id] !in setOf(Roles.EXTERNAL, Roles.LOCATION)) {
            "Invalid owner: $id"
        }
        return id
    }
    val channels = array(s, "channels")
    unique(channels.map { listOf(text(it, "name"), owner(it)) })
    check(channels.isEmpty()) { "Server registers no channels" }
    val spots = array(s, "spots")
    unique(spots.map { text(it, "id") })
    val logical = mutableListOf<String>()
    var dynamic = false
    for (spot in spots) {
        check(roles[owner(spot)] == Roles.MATCH) { "Room Spot owner must be a match node" }
        when (text(spot, "kind")) {
            "entry" ->
                check(
                    spot.has("type") &&
                        spot.path("type").isNull &&
                        !spot.has("logicalId") &&
                        !spot.has("dynamic")
                ) {
                    "Invalid Entry Spot declaration"
                }
            "user" -> {
                check(text(spot, "type") == Names.ROOM) { "Unregistered stable type" }
                if (spot.has("dynamic"))
                    check(spot.path("dynamic").isBoolean) { "Invalid dynamic flag" }
                if (spot.path("dynamic").asBoolean()) {
                    check(!spot.has("logicalId")) { "Dynamic Spot declares logicalId" }
                    dynamic = true
                } else logical += text(spot, "logicalId")
            }
            else -> error("Unregistered Spot kind")
        }
    }
    unique(logical)
    check(dynamic) { "Missing dynamic Room family" }
    for (node in nodes.filter { roles[text(it, "id")] == Roles.MATCH }) {
        check(
            spots.count {
                it.path("owner").asText() == text(node, "id") && it.path("kind").asText() == "entry"
            } == 1
        ) {
            "Match node must declare exactly one application Entry Spot"
        }
    }
    val requiredEdges =
        setOf(
            listOf("players", "lobby", "HTTP", ""),
            listOf("players", "session", "STREAM", ""),
            listOf("session", "matchA", "relay", ""),
            listOf("lobby", "location", "Find room", ""),
            listOf("matchA", "location", "Location / lease", "location-authority"),
        )
    val declaredEdges =
        array(s, "edges")
            .map { edge ->
                val from = text(edge, "from")
                val to = text(edge, "to")
                check(from in ids && to in ids) { "Unknown edge endpoint" }
                val semantic = if (edge.has("semantic")) text(edge, "semantic") else ""
                listOf(from, to, text(edge, "label"), semantic)
            }
            .toSet()
    check(declaredEdges.containsAll(requiredEdges)) {
        "Missing required edges: ${requiredEdges - declaredEdges}"
    }
    println("Structure: valid")
}
