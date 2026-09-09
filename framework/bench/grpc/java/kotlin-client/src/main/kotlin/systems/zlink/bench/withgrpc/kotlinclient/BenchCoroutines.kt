/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.future.future

/**
 * Bridge from a suspend body to the [systems.zlink.bench.withgrpc.client.BenchOperation]
 * the shared drivers submit.
 *
 * <p>The coroutine starts UNDISPATCHED, which means its body runs on the calling thread
 * until the first real suspension. That is deliberate and it is what keeps the kotlin
 * row's numbers comparable with the java row's, for two separate reasons.
 *
 * <p>First, accounting. The declared saturation instrument is `jvm_thread_cores`, the CPU
 * of the threads the harness runs its submit loop on (FB-032). The java row calls the
 * stub inline on that thread, so the submit cost lands there. A dispatched coroutine
 * would hand the submit itself to a `Dispatchers.Default` worker, the instrument would
 * read near zero on every cell, and it would no longer measure what its ceiling
 * describes -- the same failure FB-023 and FB-032 already corrected twice.
 *
 * <p>Second, latency. `request-serial` measures one round trip. A dispatch hop before the
 * request is sent adds queueing that belongs to no transport, and the cell would report
 * the dispatcher rather than the stack under test.
 *
 * <p>Continuations after the suspension resume on the scope's dispatcher. That CPU is
 * reported as the `jvm_all_thread_cores` observation, never as the declared instrument --
 * exactly as the java row's gRPC callbacks, which run on a grpc network thread through
 * `directExecutor`, are also outside the declared instrument.
 */
internal fun CoroutineScope.benchFuture(body: suspend () -> Unit): CompletableFuture<Void> {
    val completion: CompletableFuture<Unit> =
        future(start = CoroutineStart.UNDISPATCHED) { body() }
    @Suppress("UNCHECKED_CAST")
    return completion.thenApply { null } as CompletableFuture<Void>
}
