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
    val selected = if (args.isEmpty()) ScenarioList.order else args[1].split(',')
    require(selected.isNotEmpty() && selected.distinct() == selected && selected.all { it in ScenarioList.order }) {
        "Invalid scenarios"
    }
    var passed = 0
    var failed = 0
    for (scenarioId in selected) {
        try {
            val roomCapacity = if (scenarioId == "lobby") LOBBY_ROOM_CAPACITY else DEFAULT_ROOM_CAPACITY
            Nodes(root, roomCapacity).use { nodes ->
                println("Logs: ${nodes.logs}")
                nodes.start()
                scenario(scenarioId, nodes)
            }
        } catch (error: Exception) {
            failed++
            println("✗ $scenarioId")
            error.printStackTrace(System.err)
            continue
        }
        passed++
        println("✓ $scenarioId")
    }
    println("Scenarios: $passed/${selected.size} passed")
    if (failed > 0) exitProcess(1)
}
