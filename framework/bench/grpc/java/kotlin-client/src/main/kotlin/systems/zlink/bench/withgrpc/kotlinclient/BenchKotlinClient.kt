/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import systems.zlink.bench.withgrpc.client.BenchDrivers
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.client.BenchResultWriter
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication

/** One Kotlin source-A process for one request-window@1024 auxiliary cell. */
fun main(args: Array<String>) {
    val options = BenchOptions(args)
    require(options.implementation == "grpc-kotlin" ||
        options.implementation == "zlink-framework-kotlin") {
        "Kotlin auxiliary implementation must be grpc-kotlin or zlink-framework-kotlin"
    }
    require(options.scenario == "request-window" && options.payloadSizes.single() == 1024) {
        "Kotlin auxiliary cells are fixed to request-window@1024"
    }

    val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    val drivers = BenchDrivers(options)
    val ready = AtomicBoolean(false)
    val resource = arrayOfNulls<AutoCloseable>(1)
    val operation = arrayOfNulls<BenchOperation>(1)
    val holder = arrayOfNulls<BenchHttpApplication.Controller>(1)
    val controller = BenchHttpApplication.Controller(
        ready::get,
        drivers::counters,
    ) { trigger ->
        if (trigger.phase() == "warmup") {
            drivers.runWarmup(trigger, checkNotNull(operation[0]))
        } else {
            val result = drivers.runActive(trigger, checkNotNull(operation[0]))
            val streamImplementation = if (options.implementation == "grpc-kotlin") {
                "one platform submit thread; 100 coroutines share one logical-stream window; grpc-kotlin coroutine stub"
            } else {
                "one platform submit thread; 100 coroutines share one logical-stream window; zlink-framework-kotlin suspend awaitReply"
            }
            val metadata = linkedMapOf<String, Any>(
                "kotlinVersion" to KotlinVersion.CURRENT.toString(),
                "grpcKotlinVersion" to "1.4.1",
                "coroutinesVersion" to "1.9.0",
                "grpcStub" to "BenchServiceCoroutineStub",
                "frameworkCall" to "zlink-framework-kotlin awaitReply",
                "serverProcesses" to "Java target B binaries on the Java port band",
            )
            BenchResultWriter.write(
                options,
                checkNotNull(holder[0]).lastTrigger(),
                result,
                "kotlin",
                streamImplementation,
                metadata,
            )
        }
    }
    holder[0] = controller
    val http = BenchHttpApplication.start(options.triggerUrl, options.statsUrl, controller)
    try {
        if (options.implementation == "grpc-kotlin") {
            val stack = GrpcKotlinStack(options, scope)
            resource[0] = stack
            operation[0] = stack.echo()
        } else {
            val stack = FrameworkKotlinStack.create(options, scope)
            resource[0] = stack
            operation[0] = stack.request()
        }
        drivers.waitForRouteReady(checkNotNull(operation[0]), 1024)
        ready.set(true)
    } catch (error: Throwable) {
        http.close()
        throw error
    }

    Runtime.getRuntime().addShutdownHook(Thread({
        ready.set(false)
        http.close()
        resource[0]?.close()
        scope.cancel()
    }, "bench-kotlin-source-shutdown"))
    System.err.println(
        "[source] implementation=${options.implementation} trigger=${options.triggerUrl}" +
            " stats=${options.statsUrl} target=${options.targetEndpoint}",
    )
    Thread.currentThread().join()
}
