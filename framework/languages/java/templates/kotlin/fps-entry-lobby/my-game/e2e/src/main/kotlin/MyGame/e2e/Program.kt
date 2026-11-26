package MyGame.e2e

import MyGame.*
import java.nio.file.Path
import kotlin.system.exitProcess
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val root = Path.of("").toAbsolutePath().normalize()
    checkStructure(root)
    if (args.contentEquals(arrayOf("--check-structure"))) return@runBlocking

    val attach = args.firstOrNull() == "--attach"
    val options = args.drop(if (attach) 1 else 0)
    require(options.isEmpty() || (options.size == 2 && options[0] == "--scenario")) {
        "Use --scenario a,b"
    }
    val requested =
        if (options.isEmpty()) ScenarioList.order else options[1].split(',').map { it.trim() }
    require(requested.distinct() == requested && requested.all { it in ScenarioList.order }) {
        "Invalid scenarios: $requested"
    }
    val server =
        if (attach)
            checkNotNull(runningServer(root)) { "The running server stopped before attachment" }
        else null
    val isolated = mapOf("update" to "replaces a play node")
    if (server != null) {
        println("Using the running server: ${server.streams.values.joinToString()}")
    }
    val selected =
        (ScenarioList.order.filter { it in requested }).filter { id ->
            val reason = if (server != null) isolated[id] else null
            if (reason != null) println("Skipping $id: requires an isolated server ($reason)")
            reason == null
        }
    val results = ScenarioResults(root, selected)

    try {
        for (id in selected) {
            results.run(id) {
                if (server != null) {
                    val target = ScenarioTarget(server)
                    scenario(id, target)
                } else {
                    Nodes(root, logDirectory = results.logs.resolve(id)).use { nodes ->
                        println("Logs: ${nodes.logs}")
                        nodes.start()
                        val target = nodes.target()
                        scenario(id, target)
                    }
                }
            }
        }
    } catch (failure: Exception) {
        results.abort(failure)
    }
    exitProcess(if (results.finish()) 0 else 1)
}
