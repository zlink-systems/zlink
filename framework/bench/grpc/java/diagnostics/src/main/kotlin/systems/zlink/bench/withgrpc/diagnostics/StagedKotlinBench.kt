/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.diagnostics

import java.nio.charset.StandardCharsets
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.atomic.AtomicBoolean
import systems.zlink.bench.withgrpc.client.BenchDrivers
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.client.BenchResultWriter
import systems.zlink.bench.withgrpc.client.RawStack
import systems.zlink.bench.withgrpc.proto.BenchPayload
import systems.zlink.bench.withgrpc.shared.Args
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader
import systems.zlink.bench.withgrpc.shared.RawWire
import systems.zlink.contracts.core.Context
import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.sockets.RouterSocket
import systems.zlink.contracts.sockets.SubmitResult

/**
 * Kotlin A for the staged raw-binding fixture.  This deliberately does not use
 * zlink-framework-kotlin's public facade: a raw target has no Framework
 * lifecycle/admission protocol.  The full public Kotlin facade belongs only in
 * the final full-runtime measurement.  Codec ownership remains the shared Java
 * codec B selected by FixedDiagnosticStage at compilation.
 */
fun main(args: Array<String>) {
    require(Args.value(args, "--role", "source") == "source") {
        "Kotlin staged diagnostic is source A only; target B is the shared Java runtime"
    }
    val options = BenchOptions(args)
    require(options.payloadSizes.single() == StagedJavaBench.RESPONSE_BYTES) {
        "staged logical payload must be 4096 bytes"
    }
    val drivers = BenchDrivers(options)
    val ready = AtomicBoolean(false)
    val client = arrayOfNulls<KotlinRawClient>(1)
    val controllerHolder = arrayOfNulls<BenchHttpApplication.Controller>(1)
    val controller = BenchHttpApplication.Controller(
        ready::get,
        drivers::counters,
    ) { trigger ->
        val operation = checkNotNull(client[0]).operation(options.scenario)
        if (trigger.phase() == "warmup") {
            drivers.runWarmup(trigger, operation)
        } else {
            BenchResultWriter.write(
                options,
                checkNotNull(controllerHolder[0]).lastTrigger(),
                drivers.runActive(trigger, operation),
                "kotlin",
                "one Kotlin raw-binding submitter; protobuf retained; stage=${FixedDiagnosticStage.VALUE}",
                mapOf(
                    "diagnosticStage" to FixedDiagnosticStage.VALUE,
                    "sourceScope" to "Kotlin raw binding A; not the Kotlin Framework facade",
                    "targetScope" to "shared Java codec/runtime B",
                    "requestPayloadBytes" to StagedJavaBench.REQUEST_BYTES,
                    "responsePayloadBytes" to StagedJavaBench.RESPONSE_BYTES,
                    "sendPayloadBytes" to StagedJavaBench.SEND_BYTES,
                ),
            )
        }
    }
    controllerHolder[0] = controller
    val http = BenchHttpApplication.start(options.triggerUrl, options.statsUrl, controller)
    try {
        client[0] = KotlinRawClient.open(options)
        drivers.waitForRouteReady(checkNotNull(client[0]).operation(options.scenario),
            StagedJavaBench.RESPONSE_BYTES)
        ready.set(true)
    } catch (error: Throwable) {
        http.close()
        throw error
    }
    Runtime.getRuntime().addShutdownHook(Thread({
        ready.set(false)
        http.close()
        client[0]?.close()
    }, "bench-staged-kotlin-source-shutdown"))
    Thread.currentThread().join()
}

private class KotlinRawClient private constructor(
    private val context: Context,
    private val request: RawStack,
    private val send: RawStack,
    private val codec: RawStack.PayloadCodec,
) : AutoCloseable {
    companion object {
        fun open(options: BenchOptions): KotlinRawClient {
            val context = Zlink.createContext()
            val request = RawStack.create(
                context,
                options,
                "bench-staged-kotlin-request-${ProcessHandle.current().pid()}",
                StagedJavaBench.REQUEST_SERVER_ID,
                options.targetEndpoint,
            )
            val send = RawStack.create(
                context,
                options,
                "bench-staged-kotlin-send-${ProcessHandle.current().pid()}",
                StagedJavaBench.SEND_SERVER_ID,
                options.targetCommandEndpoint,
            )
            return KotlinRawClient(
                context,
                request,
                send,
                StagedJavaBench.sourceCodec(options.runId),
            )
        }
    }

    fun operation(scenario: String): BenchOperation =
        if (scenario == "send-saturation") send.send(codec)
        else request.request(StagedJavaBench.REQUEST_BYTES, StagedJavaBench.RESPONSE_BYTES, codec)

    override fun close() {
        request.close()
        send.close()
        context.close()
    }
}
