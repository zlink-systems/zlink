package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    checkStructure(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return@runBlocking
    require(args.isEmpty() || (args.size == 2 && args[0] == "--scenario"))
    val selected = if (args.isEmpty()) ScenarioList.order else args[1].split(',').map { it.trim() }
    require(
        selected.isNotEmpty() &&
            selected.distinct() == selected &&
            selected.all { it in ScenarioList.order }
    )
    var passed = 0
    for (id in ScenarioList.order.filter { it in selected }) {
        try {
            Nodes(root).use { nodes ->
                println("Logs: ${nodes.logs}")
                nodes.start()
                scenario(id, nodes)
            }
            passed++
            println("✓ $id")
        } catch (failure: Exception) {
            println("✗ $id")
            failure.printStackTrace(System.err)
        }
    }
    println("Scenarios: $passed/${selected.size} passed")
    exitProcess(if (passed == selected.size) 0 else 1)
}
