package systems.zlink.framework.perf.kotlin

import java.util.concurrent.CompletionStage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.future

// Java ServerApplication accepts a CompletionStage workload; this is the single Kotlin bridge from its
// suspending scenario bodies to that harness contract. Framework calls themselves use their public wrappers.
fun completionStage(block: suspend () -> Unit): CompletionStage<Void> =
    CoroutineScope(Dispatchers.IO).future {
        block()
        null
    }
