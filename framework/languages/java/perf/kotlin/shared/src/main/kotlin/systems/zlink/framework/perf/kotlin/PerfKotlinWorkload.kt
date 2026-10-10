package systems.zlink.framework.perf.kotlin

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch

// Each stream advances after its call terminal: local reply or remote one-way admission.
public suspend fun runTerminalStreams(
    streamCount: Int,
    canIssue: () -> Boolean,
    operation: suspend (Int) -> Boolean,
) = coroutineScope {
    repeat(streamCount) { stream ->
        launch(Dispatchers.IO) {
            while (canIssue()) {
                if (!operation(stream)) break
            }
        }
    }
}

public fun planStreamTargets(targets: List<String>, streamCount: Int): List<String> {
    require(targets.isNotEmpty()) { "A workload requires at least one target." }
    return List(streamCount) { stream -> targets[stream % targets.size] }
}
