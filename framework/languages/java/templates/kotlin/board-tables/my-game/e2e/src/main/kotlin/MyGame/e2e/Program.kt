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
    if (attach && scenarioArgs.isEmpty()) {
        selected = selected.filter { it != "update" }
        println("Running against the started server; update runs when no server is running.")
    }
    var passed = 0
    suspend fun runScenarios(endpoint: String, nodes: Nodes? = null) {
        selected.forEachIndexed { index, id ->
            println("━━ $id (${index + 1}/${selected.size}) ━━")
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
                require("update" !in selected) {
                    "The update scenario requires an isolated server."
                }
                val endpoint =
                    checkNotNull(runningEndpoint(root)) {
                        "Start the server with run-server before attaching."
                    }
                println("Attached to Session STREAM: $endpoint")
                runScenarios(endpoint)
            } else {
                Nodes(root, selected.size).use { nodes ->
                    println("Logs: ${nodes.logs}")
                    nodes.start()
                    runScenarios(nodes.endpoint, nodes)
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
