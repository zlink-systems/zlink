package MyGame.e2e

import MyGame.Names
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    checkStructure(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return@runBlocking
    val attach = args.firstOrNull() == "--attach"
    val scenarioArgs = args.drop(if (attach) 1 else 0)
    require(scenarioArgs.isEmpty() || (scenarioArgs.size == 2 && scenarioArgs[0] == "--scenario")) {
        "Use [--attach] [--scenario a,b]"
    }
    val requested = if (scenarioArgs.isEmpty()) Names.SCENARIOS else scenarioArgs[1].split(',')
    require(requested.distinct() == requested && requested.all { it in Names.SCENARIOS }) {
        "Invalid scenarios: $requested"
    }
    var selected = Names.SCENARIOS.filter { it in requested }
    val hostScenarios = setOf("update", "persist")
    if (attach && scenarioArgs.isEmpty()) {
        selected = selected.filterNot { it in hostScenarios }
        println("Attached runs leave the server running; update and persist run in isolation.")
    }
    var passed = 0
    var started = 0
    suspend fun runScenarios(scenarios: List<String>, endpoint: String, nodes: Nodes? = null) {
        for (id in scenarios) {
            println("━━ $id (${++started}/${selected.size}) ━━")
            try {
                scenario(id, endpoint, nodes)
                passed++
                println("✓ $id")
            } catch (failure: Exception) {
                println("✗ $id")
                failure.printStackTrace(System.err)
            }
        }
    }
    val completed =
        try {
            if (attach) {
                require(selected.none { it in hostScenarios }) {
                    "The update and persist scenarios require an isolated server."
                }
                val endpoint =
                    checkNotNull(runningEndpoint(root)) {
                        "Start the server with run-server before attaching."
                    }
                println("Attached to Session STREAM: $endpoint")
                runScenarios(selected, endpoint)
            } else {
                // Host replacement in persist must leave the other scenarios' placement intact.
                val deployments = selected.partition { it != "persist" }
                for (scenarios in listOf(deployments.first, deployments.second)) {
                    if (scenarios.isEmpty()) continue
                    Nodes(root, scenarios.size).use { nodes ->
                        println("Logs: ${nodes.logs}")
                        nodes.start()
                        runScenarios(scenarios, nodes.endpoint, nodes)
                    }
                }
            }
            true
        } catch (failure: Exception) {
            failure.printStackTrace(System.err)
            false
        }
    println("Scenarios: $passed/${selected.size} passed")
    exitProcess(if (completed && passed == selected.size) 0 else 1)
}
