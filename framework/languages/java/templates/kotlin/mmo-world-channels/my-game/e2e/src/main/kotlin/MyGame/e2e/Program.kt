package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) {
    val root = Path.of("").toAbsolutePath().normalize()
    Structure.check(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return
    require(args.isEmpty() || (args.size == 2 && args[0] == "--scenario")) { "Use --scenario a,b" }
    val selected =
        (if (args.isEmpty()) Names.SCENARIOS else args[1].split(',')).map(Scenario::parse)
    require(selected.isNotEmpty() && selected.toSet().size == selected.size) { "Invalid scenarios" }
    var passed = 0
    runBlocking {
        for (id in selected) {
            val nodes = Nodes(root)
            var success = false
            try {
                println("Logs: ${nodes.logs}")
                nodes.start()
                Scenarios.run(id, nodes)
                success = true
            } catch (error: Exception) {
                error.printStackTrace(System.err)
            } finally {
                try {
                    nodes.close()
                } catch (error: Exception) {
                    success = false
                    error.printStackTrace(System.err)
                }
            }
            if (success) passed++
            println((if (success) "✓ " else "✗ ") + id.id)
        }
    }
    println("Scenarios: $passed/${selected.size} passed")
    exitProcess(if (passed == selected.size) 0 else 1)
}
