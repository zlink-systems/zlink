package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    checkStructure(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return@runBlocking
    require(args.isEmpty() || (args.size == 2 && args[0] == "--scenario")) { "Use --scenario a,b" }
    val requested =
        (if (args.isEmpty()) Names.SCENARIOS else args[1].split(',')).map {
            Scenario.parse(it.trim())
        }
    require(requested.isNotEmpty() && requested.distinct() == requested) { "Invalid scenarios" }
    val selected = Scenario.entries.filter { it in requested }
    var passed = 0
    var cleanupFailure: Exception? = null
    val nodes = Nodes(root)
    try {
        println("Logs: ${nodes.logs}")
        nodes.start()
        for (scenario in selected) {
            try {
                runScenario(scenario, nodes)
                passed++
                println("✓ ${scenario.id}")
            } catch (failure: Exception) {
                println("✗ ${scenario.id}")
                failure.printStackTrace(System.err)
            }
        }
    } catch (failure: Exception) {
        failure.printStackTrace(System.err)
    } finally {
        try {
            nodes.close()
        } catch (failure: Exception) {
            failure.printStackTrace(System.err)
            cleanupFailure = failure
        }
    }
    println("Scenarios: $passed/${selected.size} passed")
    exitProcess(if (passed == selected.size && cleanupFailure == null) 0 else 1)
}
