package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    if (args.contentEquals(arrayOf("--check-structure"))) {
        checkStructure(root)
        return@runBlocking
    }
    DomainChecks.run()
    if (args.contentEquals(arrayOf("--check-domain"))) return@runBlocking
    checkStructure(root)
    require(args.isEmpty() || (args.size == 2 && args[0] == "--scenario"))
    val selected = if (args.isEmpty()) Names.SCENARIOS else args[1].split(',')
    require(
        selected.isNotEmpty() &&
            selected.distinct() == selected &&
            selected.all { it in Names.SCENARIOS }
    )
    var passed = 0
    var cleanupFailure: Exception? = null
    val nodes = Nodes(root)
    val scenarios = Scenarios(nodes, selected)
    try {
        println("Logs: ${nodes.logs}")
        nodes.start()
        for (id in Names.SCENARIOS.filter { it in selected }) {
            try {
                scenarios.run(id)
                passed++
                println("✓ $id")
            } catch (failure: Exception) {
                println("✗ $id")
                failure.printStackTrace(System.err)
            }
        }
    } catch (failure: Exception) {
        failure.printStackTrace(System.err)
    } finally {
        try {
            scenarios.close()
        } catch (failure: Exception) {
            failure.printStackTrace(System.err)
            cleanupFailure = failure
        }
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
