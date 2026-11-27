package MyGame.e2e

import java.nio.file.Files
import java.nio.file.Path
import java.util.UUID
import kotlinx.coroutines.CancellationException

class ScenarioResults(root: Path, private val selected: List<String>) {
    val logs = root.resolve(".logs/run-${UUID.randomUUID()}")
    private var started = 0
    private var passed = 0
    private var aborted = false

    init {
        Files.createDirectories(logs)
        println("Scenario logs: $logs")
    }

    suspend fun run(id: String, stopOnFailure: Boolean = false, action: suspend () -> Unit) {
        heading(id)
        try {
            action()
            passed++
            println("✓ $id")
        } catch (failure: CancellationException) {
            throw failure
        } catch (failure: Exception) {
            report(id, failure)
            if (stopOnFailure) throw failure
        }
    }

    fun abort(failure: Exception) {
        aborted = true
        report("Run", failure)
        for (id in selected.drop(started)) {
            heading(id)
            report(id, failure)
        }
    }

    fun finish(): Boolean {
        println("Scenarios: $passed/${selected.size} passed")
        return !aborted && passed == selected.size
    }

    private fun heading(id: String) {
        println("━━ $id (${++started}/${selected.size}) ━━")
    }

    private fun report(id: String, failure: Exception) {
        val detail = logs.resolve("$id-failure.log")
        Files.writeString(detail, failure.stackTraceToString())
        System.err.println("✗ $id: ${failure.toString().lineSequence().first()} ($detail)")
    }
}
