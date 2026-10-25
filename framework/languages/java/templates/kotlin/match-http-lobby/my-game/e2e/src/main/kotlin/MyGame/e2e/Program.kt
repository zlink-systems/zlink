package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    checkStructure(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return@runBlocking
    LobbyDomainChecks.run()
    if (args.contentEquals(arrayOf("--check-domain"))) return@runBlocking
    require(args.isEmpty() || (args.size == 2 && args[0] == "--scenario")) { "Use --scenario a,b" }
    val selected =
        (if (args.isEmpty()) Names.SCENARIOS else args[1].split(',')).map(Scenario::parse)
    require(selected.isNotEmpty() && selected.distinct() == selected) { "Invalid scenarios" }
    var passed = 0
    for (scenario in selected) {
        try {
            Nodes(root).use { nodes ->
                println("Logs: ${nodes.logs}")
                nodes.start()
                scenario(scenario.id, nodes)
            }
        } catch (error: Exception) {
            println("✗ ${scenario.id}")
            error.printStackTrace(System.err)
            println("Scenarios: $passed/${selected.size} passed")
            exitProcess(1)
        }
        passed++
        println("✓ ${scenario.id}")
    }
    println("Scenarios: $passed/${selected.size} passed")
}
