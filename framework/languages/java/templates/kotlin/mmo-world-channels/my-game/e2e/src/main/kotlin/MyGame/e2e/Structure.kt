package MyGame.e2e

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import java.nio.file.Files
import java.nio.file.Path

object Structure {
    private fun text(node: JsonNode, key: String): String {
        val value = node.get(key)
        check(value != null && value.isTextual && value.asText().isNotBlank()) {
            "Missing string: $key"
        }
        return value.asText()
    }

    private fun array(node: JsonNode, key: String): List<JsonNode> {
        val value = node.get(key)
        check(value != null && value.isArray) { "Missing array: $key" }
        return value.toList()
    }

    private fun unique(values: List<*>) {
        check(values.toSet().size == values.size) { "Duplicate identity: $values" }
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

    private fun owner(node: JsonNode, roles: Map<String, String>): String {
        val id = text(node, "owner")
        check(id in roles && roles[id] !in Roles.WITHOUT_SPOTS) { "Invalid owner: $id" }
        return id
    }

    fun check(root: Path) {
        val s = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
        check(s.path("version").isIntegralNumber && s.path("version").asInt() == 1) {
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
        val keys = commands.fieldNames().asSequence().toSet()
        check(keys == setOf("sh", "powershell")) { "Invalid command shells" }
        keys.forEach { shell ->
            val values = array(commands, shell)
            check(values.isNotEmpty() && values.all { it.isTextual && it.asText().isNotBlank() }) {
                "Invalid commands"
            }
        }
        val nodes = array(s, "nodes")
        val ids = nodes.map { text(it, "id") }
        unique(ids)
        val roles = nodes.associate { text(it, "id") to text(it, "role") }
        val configs = mutableMapOf<String, NodeConfig>()
        nodes.forEach { node ->
            val id = text(node, "id")
            val role = roles.getValue(id)
            text(node, "title")
            check(node.has("config")) { "Missing config" }
            if (role == Roles.EXTERNAL)
                check(node.path("config").isNull) { "External config must be null" }
            else {
                val file = configPath(root, text(node, "config"))
                if (role !in setOf(Roles.WORLD_DB, Roles.LOCATION_STORE)) {
                    val config = Json.mapper.readValue(file.toFile(), NodeConfig::class.java)
                    check(config.id == id) { "Configuration Id mismatch: $file" }
                    configs[id] = config
                }
            }
        }
        val channels = array(s, "channels")
        check(channels.isEmpty()) { "Server registers no channels" }
        val spots = array(s, "spots")
        unique(spots.map { text(it, "id") })
        val logical = mutableListOf<String>()
        spots.forEach { spot ->
            val id = owner(spot, roles)
            check(roles[id] == Roles.CHANNEL) { "Zone Spot owner must be a channel node" }
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
                    check(text(spot, "type") == Names.ZONE) { "Unregistered stable type" }
                    if (spot.has("dynamic"))
                        check(spot.path("dynamic").isBoolean) { "Invalid dynamic flag" }
                    check(!spot.path("dynamic").asBoolean()) { "Zones must have fixed logical IDs" }
                    val config = configs.getValue(id)
                    val logicalId = text(spot, "logicalId")
                    check(text(spot, "mesh") == Names.mesh(requireNotNull(config.worldId))) {
                        "Zone mesh mismatch"
                    }
                    check(logicalId in config.zoneIds) { "Zone absent from owner configuration" }
                    Names.channel(logicalId)
                    logical += text(spot, "mesh") + ":" + logicalId
                }
                else -> error("Unregistered Spot kind")
            }
        }
        unique(logical)
        nodes
            .filter { roles[text(it, "id")] == Roles.CHANNEL }
            .forEach { node ->
                val config = configs.getValue(text(node, "id"))
                val owned = spots.filter { it.path("owner").asText() == config.id }
                check(owned.count { it.path("kind").asText() == "entry" } == 1) {
                    "Channel node requires one Entry Spot"
                }
                val actual =
                    owned
                        .filter { it.path("kind").asText() == "user" }
                        .map { it.path("logicalId").asText() }
                        .sorted()
                check(actual == config.zoneIds.sorted()) {
                    "Configured Zone set differs from structure"
                }
            }
        val requiredEdges =
            setOf(
                listOf("players", "accounts", "HTTP", ""),
                listOf("players", "sessionA", "STREAM", ""),
                listOf("players", "sessionB", "STREAM", ""),
                listOf("sessionA", "channelA1", "relay", ""),
                listOf("sessionB", "channelB", "relay", ""),
                listOf("channelA1", "channelA2", "relocation", ""),
                listOf("channelA2", "dbA", "game data", ""),
                listOf("channelB", "dbB", "game data", ""),
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
}
