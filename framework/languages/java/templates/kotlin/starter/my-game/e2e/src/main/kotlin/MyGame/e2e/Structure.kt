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
    fun array(node: JsonNode, key: String): List<JsonNode> {
        check(node[key]?.isArray == true) { "Missing array: $key" }
        return node[key].toList()
    }
    fun path(value: String): Path {
        val relative = Path.of(value)
        check(
            !relative.isAbsolute && '\\' !in value && !Regex("^[A-Za-z]:").containsMatchIn(value)
        ) {
            "Invalid relative path: $value"
        }
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
    check(structure["version"]?.isIntegralNumber == true && structure["version"].asInt() == 1) {
        "Unsupported structure version"
    }
    for (key in listOf("id", "name")) text(structure, key)
    check(text(structure, "language") == Names.LANGUAGE) {
        "Structure language differs from server"
    }
    val run = structure["run"] ?: error("Missing run")
    check(array(run, "scenarios").map { it.asText() } == Names.SCENARIOS) {
        "Scenario list differs from runner"
    }
    val commands = run["commands"] ?: error("Missing commands")
    check(commands.fieldNames().asSequence().toSet() == setOf("sh", "powershell")) {
        "Invalid command shells"
    }
    for (shell in listOf("sh", "powershell")) check(
        array(commands, shell).isNotEmpty() &&
            array(commands, shell).all { it.isTextual && it.asText().isNotBlank() }
    ) {
        "Invalid commands"
    }
    val ids = mutableSetOf<String>()
    var sessions = 0
    for (node in array(structure, "nodes")) {
        val id = text(node, "id")
        check(ids.add(id)) { "Duplicate node ID: $id" }
        val role = text(node, "role")
        text(node, "title")
        check(node.has("config")) { "Missing config" }
        if (role == Roles.EXTERNAL) {
            check(node["config"].isNull) { "External config must be null" }
            continue
        }
        val file = path(text(node, "config"))
        if (role == Roles.LOCATION) continue
        check(role == Roles.SESSION) { "No entry point is registered for role: $role" }
        // Reading NodeConfig also checks the required STREAM endpoint.
        val config = Json.mapper.readValue(file.toFile(), NodeConfig::class.java)
        check(config.id == id) { "Configuration Id mismatch: $file" }
        sessions++
    }
    check(sessions > 0) { "At least one application node must be declared" }
    check(array(structure, "channels").isEmpty()) { "No application channels are registered" }
    check(array(structure, "spots").isEmpty()) { "No application Spots are registered" }
    for (edge in array(structure, "edges")) {
        check(text(edge, "from") in ids && text(edge, "to") in ids) { "Unknown edge endpoint" }
        text(edge, "label")
        if (edge.has("semantic")) text(edge, "semantic")
    }
    println("Structure: valid")
}
