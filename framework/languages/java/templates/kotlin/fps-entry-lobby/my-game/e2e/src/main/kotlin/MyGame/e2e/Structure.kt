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
    check(
        structure["version"]?.isIntegralNumber == true &&
            structure["version"].canConvertToInt() &&
            structure["version"].asInt() == 1
    )
    for (key in listOf("id", "name", "language")) text(structure, key)
    check(text(structure, "id") == "fps-entry-lobby") { "Structure id differs from template" }
    check(text(structure, "language") == Names.LANGUAGE) {
        "Structure language differs from server"
    }
    val run = requireNotNull(structure["run"])
    check(
        run["scenarios"]?.isArray == true && run["scenarios"].map { it.asText() } == ScenarioList.order
    )
    val commands = requireNotNull(run["commands"])
    check(commands.fieldNames().asSequence().toSet() == setOf("sh", "powershell"))
    val scenarioArgument = ScenarioList.order.joinToString(",")
    check(commands["sh"].isArray && commands["sh"].map { it.asText() } == listOf(
        "./run-e2e.sh --build-only", "./run-e2e.sh", "./run-e2e.sh --scenario $scenarioArgument",
    )) { "Invalid shell commands" }
    check(commands["powershell"].isArray && commands["powershell"].map { it.asText() } == listOf(
        "pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly", "pwsh -NoProfile -File ./run-e2e.ps1",
        "pwsh -NoProfile -File ./run-e2e.ps1 -Scenario $scenarioArgument",
    )) { "Invalid PowerShell commands" }
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
    for (channel in channels) error("No application channel is registered: $channel")
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
    for (spot in spots) {
        check(roles[owner(spot)] in listOf("match"))
        when (text(spot, "kind")) {
            "entry" ->
                check(
                    spot["type"]?.isNull == true && !spot.has("logicalId") && !spot.has("dynamic")
                )
            "user" -> {
                check(text(spot, "type") in listOf(Names.ROOM, Names.MATCHMAKING))
                if (spot["dynamic"]?.asBoolean() == true) check(!spot.has("logicalId"))
                else logicalIds += text(spot, "logicalId")
                if (spot.has("dynamic")) check(spot["dynamic"].isBoolean)
            }
            else -> error("Unregistered Spot kind")
        }
    }
    check(spots.any { it["type"]?.asText() == Names.ROOM && it["dynamic"]?.asBoolean() == true }) {
        "Missing dynamic Room family"
    }
    unique(logicalIds)
    val bootstraps = configs.filterValues { it.bootstrap }.keys
    val matchmakingOwners =
        spots
            .filter { it["type"]?.asText() == Names.MATCHMAKING }
            .map {
                check(
                    it["dynamic"]?.asBoolean() != true &&
                        text(it, "logicalId") == Names.MATCHMAKING_ID
                )
                owner(it)
            }
            .toSet()
    check(matchmakingOwners.isNotEmpty() && bootstraps == matchmakingOwners) {
        "Bootstrap differs from fixed Matchmaking owner"
    }
    for ((id, config) in configs.filterKeys { roles[it] == "match" }) {
        check(config.roomCapacity > 0)
        check(spots.count { it["owner"].asText() == id && it["kind"].asText() == "entry" } == 1)
    }
    val requiredEdges =
        setOf(
            listOf("players", "session", "STREAM", ""),
            listOf("session", "matchA", "relay", ""),
            listOf("matchA", "location", "Location / lease", "location-authority"),
        )
    val declaredEdges =
        array("edges")
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
