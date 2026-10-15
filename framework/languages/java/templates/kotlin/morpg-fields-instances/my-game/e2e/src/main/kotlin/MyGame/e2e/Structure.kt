package MyGame.e2e

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import java.nio.file.Files
import java.nio.file.Path

fun checkStructure(root: Path) {
    val structure = Json.mapper.readTree(root.resolve("zlink.structure.json").toFile())
    fun text(node: JsonNode, key: String): String {
        check(node[key]?.isTextual == true && node[key].asText().isNotBlank()) {
            "Missing string: $key"
        }
        return node[key].asText()
    }
    fun array(key: String): List<JsonNode> {
        check(structure[key]?.isArray == true) { "Missing array: $key" }
        return structure[key].toList()
    }
    fun unique(values: List<Any>) {
        check(values.toSet().size == values.size) { "Duplicate identity: $values" }
    }
    fun path(value: String): Path {
        val relative = Path.of(value)
        check(!relative.isAbsolute && '\\' !in value && !Regex("^[A-Za-z]:").containsMatchIn(value))
        val resolved = root.resolve(relative).normalize()
        check(
            resolved.startsWith(root) &&
                Files.isRegularFile(resolved) &&
                resolved.toRealPath().startsWith(root.toRealPath())
        ) {
            "Invalid configuration path: $value"
        }
        return resolved
    }
    check(structure["version"]?.isIntegralNumber == true && structure["version"].asInt() == 1)
    for (key in listOf("id", "name", "language")) text(structure, key)
    check(text(structure, "language") == Names.LANGUAGE) {
        "Structure language differs from server"
    }
    val run = requireNotNull(structure["run"])
    check(
        run["scenarios"]?.isArray == true && run["scenarios"].map { it.asText() } == Names.SCENARIOS
    )
    val commands = requireNotNull(run["commands"])
    check(commands.fieldNames().asSequence().toSet() == setOf("sh", "powershell"))
    for (shell in listOf("sh", "powershell")) check(
        commands[shell].isArray &&
            !commands[shell].isEmpty &&
            commands[shell].all { it.isTextual && it.asText().isNotBlank() }
    )
    val nodes = array("nodes")
    val ids = nodes.map { text(it, "id") }
    unique(ids)
    val roles = nodes.associate { text(it, "id") to text(it, "role") }
    for (node in nodes) {
        text(node, "title")
        val role = text(node, "role")
        check(node.has("config"))
        if (role == "external") check(node["config"].isNull)
        else {
            val file = path(text(node, "config"))
            if (role != "location-store")
                check(Json.mapper.readTree(file.toFile())["id"]?.asText() == text(node, "id")) {
                    "Configuration Id mismatch: $file"
                }
        }
    }
    fun owner(node: JsonNode): String {
        val id = text(node, "owner")
        check(id in ids && roles[id] !in listOf("external", "location-store"))
        return id
    }
    val channels = array("channels")
    unique(channels.map { text(it, "name") to owner(it) })
    for (channel in channels) {
        check(text(channel, "name") == Names.BORDER && text(channel, "mesh") == Names.MESH)
        check(roles[owner(channel)] == "field")
    }
    val configs =
        nodes
            .filter { roles[it["id"].asText()] !in listOf("external", "location-store") }
            .associate {
                text(it, "id") to
                    Json.mapper.readValue(path(text(it, "config")).toFile(), NodeConfig::class.java)
            }
    val spots = array("spots")
    unique(spots.map { text(it, "id") })
    val logicalIds = mutableListOf<String>()
    var dynamicDungeon = false
    for (spot in spots) {
        check(roles[owner(spot)] in listOf("field", "instance"))
        when (text(spot, "kind")) {
            "entry" ->
                check(
                    roles[owner(spot)] == "field" &&
                        spot["type"]?.isNull == true &&
                        !spot.has("logicalId") &&
                        !spot.has("dynamic")
                )
            "user" -> {
                check(
                    text(spot, "type") ==
                        if (roles[owner(spot)] == "field") Names.FIELD else Names.DUNGEON
                )
                if (spot["dynamic"]?.asBoolean() == true) {
                    check(!spot.has("logicalId") && text(spot, "type") == Names.DUNGEON)
                    dynamicDungeon = true
                } else logicalIds += text(spot, "logicalId")
                if (spot.has("dynamic")) check(spot["dynamic"].isBoolean)
            }
            else -> error("Unregistered Spot kind")
        }
    }
    check(dynamicDungeon) { "Missing dynamic Dungeon family" }
    unique(logicalIds)
    for (node in nodes.filter { roles[it["id"].asText()] == "field" }) {
        val id = text(node, "id")
        check(spots.any { it["owner"].asText() == id && it["kind"].asText() == "entry" })
        val configured = requireNotNull(configs[id]).fieldIds
        val declared =
            spots
                .filter {
                    it["owner"].asText() == id &&
                        it["type"]?.asText() == Names.FIELD &&
                        it["dynamic"]?.asBoolean() != true
                }
                .map { text(it, "logicalId") }
        unique(configured)
        check(configured.toSet() == declared.toSet()) {
            "Configured FieldIds differ from fixed Spots: $id"
        }
        check(channels.any { it["owner"].asText() == id }) { "Missing border channel: $id" }
    }
    for (edge in array("edges")) {
        check(text(edge, "from") in ids && text(edge, "to") in ids)
        text(edge, "label")
        if (edge.has("semantic")) text(edge, "semantic")
    }
    println("Structure: valid")
}
