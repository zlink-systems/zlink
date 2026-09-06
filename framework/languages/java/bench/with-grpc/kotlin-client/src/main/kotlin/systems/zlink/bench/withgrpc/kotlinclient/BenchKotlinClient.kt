/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.time.ZonedDateTime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import systems.zlink.bench.withgrpc.client.BenchDrivers
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.client.ClientResources
import systems.zlink.bench.withgrpc.client.StatsClient
import systems.zlink.bench.withgrpc.shared.BenchContract
import systems.zlink.bench.withgrpc.shared.RawWire
import systems.zlink.contracts.core.Context
import systems.zlink.contracts.core.Zlink

/**
 * with-grpc local bench, kotlin client. One client process and one server process per
 * implementation (spec section 3), on the kotlin port band of spec section 9.
 *
 * <p>Emits `cells.json` in the `with-grpc-cell-v1` shape plus RESULT lines. Every table,
 * ratio and verdict is produced by framework/bench/tools, never here (plan section 4.1,
 * FB-020). The drivers are the java row's: this file orchestrates cells and records
 * kotlin provenance, it does not measure.
 */
class BenchKotlinClient(private val options: BenchOptions) {
    private val stats = StatsClient()
    private val drivers = BenchDrivers(options, stats)
    private val cells = mutableListOf<Map<String, Any?>>()
    private val failures = mutableListOf<String>()
    private val contaminated = mutableListOf<String>()
    private val drainObservations = mutableListOf<String>()
    private val pendingContamination = mutableMapOf<String, String>()

    // One scope for every measured coroutine. Dispatchers.Default is the dispatcher a
    // kotlin service would use for this work; the coroutines start undispatched so the
    // submit itself still runs on the driver's thread (see BenchCoroutines).
    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())

    fun run(): Int {
        Files.createDirectories(Path.of(options.output))
        val context: Context = Zlink.createContext()
        val grpc = if (options.runsImplementation(GRPC)) GrpcKotlinStack(options, scope) else null

        // The framework host is built once, outside every measured window. A failure to
        // stand it up marks the six framework cells unsupported with the reason rather
        // than taking the run down.
        var framework: FrameworkKotlinStack? = null
        var frameworkError: String? = null
        if (options.runsImplementation(FRAMEWORK)) {
            try {
                framework = FrameworkKotlinStack.create(options, scope)
            } catch (error: Exception) {
                frameworkError = error.toString()
                System.err.println("[bench] framework client unavailable: $frameworkError")
            }
        }

        for (payloadSize in options.payloadSizes) {
            for (pattern in PATTERNS) {
                if (!options.runsPattern(pattern)) {
                    continue
                }
                if (grpc != null) {
                    runGrpc(grpc, pattern, payloadSize)
                }
                if (options.runsImplementation(RAW)) {
                    runRaw(context, pattern, payloadSize)
                }
                if (options.runsImplementation(FRAMEWORK)) {
                    runFramework(framework, frameworkError, pattern, payloadSize)
                }
            }
        }

        writeOutputs()
        System.err.println(
            "[bench] cells completed=${cells.size} failed=${failures.size}" +
                " contaminated=${contaminated.size}",
        )
        grpc?.close()
        framework?.close()
        scope.cancel()
        context.close()
        return 0
    }

    private fun runGrpc(grpc: GrpcKotlinStack, pattern: String, payloadSize: Int) {
        addCell(GRPC, pattern, payloadSize) {
            when (pattern) {
                "request-serial" ->
                    drivers.runRequestSerial(payloadSize, options.grpcStatsUrl, grpc.echo())
                "request-window" ->
                    drivers.runRequestWindow(payloadSize, options.grpcStatsUrl, grpc.echo())
                else ->
                    drivers.runSendSaturation(payloadSize, options.grpcStatsUrl, grpc.command())
            }
        }
    }

    private fun runRaw(context: Context, pattern: String, payloadSize: Int) {
        addCell(RAW, pattern, payloadSize) {
            val isSend = pattern == "send-saturation"
            val peer =
                if (isSend) RawWire.RAW_COMMAND_SERVER_ID else RawWire.RAW_REQUEST_SERVER_ID
            val endpoint =
                if (isSend) options.zlinkRawCommandEndpoint else options.zlinkRawEndpoint
            val selfId = "benchk-${pattern[8]}-${ProcessHandle.current().pid()}-s$payloadSize"
            RawKotlinStack.create(context, options, selfId, peer, endpoint, scope).use { raw ->
                val operation: BenchOperation = if (isSend) raw.send() else raw.request()
                drivers.waitForRouteReady(operation, payloadSize)
                when (pattern) {
                    "request-serial" ->
                        drivers.runRequestSerial(payloadSize, options.zlinkRawStatsUrl, operation)
                    "request-window" ->
                        drivers.runRequestWindow(payloadSize, options.zlinkRawStatsUrl, operation)
                    else ->
                        drivers.runSendSaturation(
                            payloadSize, options.zlinkRawStatsUrl, operation,
                        )
                }
            }
        }
    }

    private fun runFramework(
        framework: FrameworkKotlinStack?,
        frameworkError: String?,
        pattern: String,
        payloadSize: Int,
    ) {
        addCell(FRAMEWORK, pattern, payloadSize) {
            checkNotNull(framework) {
                "framework host unavailable via zlink-framework-spring-boot-starter:" +
                    " $frameworkError"
            }
            val operation: BenchOperation =
                if (pattern == "send-saturation") framework.send() else framework.request()
            drivers.waitForRouteReady(operation, payloadSize)
            when (pattern) {
                "request-serial" ->
                    drivers.runRequestSerial(payloadSize, options.zlinkStatsUrl, operation)
                "request-window" ->
                    drivers.runRequestWindow(payloadSize, options.zlinkStatsUrl, operation)
                else ->
                    drivers.runSendSaturation(payloadSize, options.zlinkStatsUrl, operation)
            }
        }
    }

    /**
     * Cell isolation: one cell that throws must not take the other 17 with it. No retry
     * and no fabricated value -- a failed cell is simply absent, which the aggregator
     * reads as `unsupported`.
     */
    private fun addCell(
        implementation: String,
        pattern: String,
        payloadSize: Int,
        body: () -> Map<String, Any?>,
    ) {
        val name = "$implementation-$pattern"
        val reason = pendingContamination.remove(implementation)
        if (reason != null) {
            contaminated.add("$name@$payloadSize: $reason")
            System.err.println("[bench] CONTAMINATED $name@$payloadSize: $reason")
            return
        }
        System.err.println("[bench] running $name@$payloadSize")
        try {
            val measured = body()
            val cell = LinkedHashMap<String, Any?>()
            cell["implementation"] = implementation
            cell["pattern"] = pattern
            cell["payload_size"] = payloadSize
            cell["contaminated"] = false
            cell["contamination_reason"] = null
            cell.putAll(measured)
            cells.add(cell)
            val drainMsValue = measured["drain_ms"]
            if (drainMsValue is Number) {
                val boundHit = measured["drain_bound_hit"] == true
                drainObservations.add(
                    "%s@%d: drain_ms=%.0f bound_hit=%s bound_ms=%d".format(
                        name, payloadSize, drainMsValue.toDouble(), boundHit,
                        options.drainBoundMs,
                    ),
                )
                if (boundHit) {
                    // FB-008: the next cell on this same server stands behind the backlog
                    // this one left. Mark it and exclude it rather than measuring it.
                    pendingContamination[implementation] =
                        ("previous cell %s@%d did not drain within %dms (observed %.0fms)")
                            .format(
                                name, payloadSize, options.drainBoundMs,
                                drainMsValue.toDouble(),
                            )
                }
            }
            System.err.println(
                "[bench] finished %s@%d throughput=%.1f/s cores=%.2f peak_in_flight=%s".format(
                    name, payloadSize,
                    (measured["throughput_per_second"] as Number).toDouble(),
                    (measured["client_cores"] as Number).toDouble(),
                    measured["peak_in_flight"],
                ),
            )
        } catch (error: Throwable) {
            // The cause chain is kept: a bare toString() hides which layer refused, and a
            // failed cell has to be explainable from the run artefacts alone.
            failures.add("$name@$payloadSize: ${describe(error)}")
            System.err.println("[bench] FAILED $name@$payloadSize: ${describe(error)}")
            error.printStackTrace()
        }
    }

    private fun describe(error: Throwable): String {
        val out = StringBuilder()
        var current: Throwable? = error
        while (current != null) {
            if (out.isNotEmpty()) {
                out.append(" <- ")
            }
            out.append(current.toString())
            current = if (current.cause === current) null else current.cause
        }
        return out.toString()
    }

    // --- outputs -----------------------------------------------------------

    private fun writeOutputs() {
        val metadata = collectMetadata()
        val json = StringBuilder()
        json.append("{\n  \"schema\": \"with-grpc-cell-v1\",\n  \"metadata\": ")
        BenchJson.write(json, metadata, 1)
        json.append(",\n  \"cells\": [\n")
        cells.forEachIndexed { index, cell ->
            json.append("    ")
            BenchJson.write(json, cell, 2)
            json.append(if (index + 1 < cells.size) ",\n" else "\n")
        }
        json.append("  ]\n}\n")
        Files.writeString(
            Path.of(options.output, "cells.json"), json.toString(), StandardCharsets.UTF_8,
        )

        val lines = mutableListOf("# with-grpc bench, kotlin row")
        for ((key, value) in metadata) {
            if (value is Map<*, *> || value is List<*>) {
                continue
            }
            lines.add("  $key: $value")
        }
        lines.add("")
        for (cell in cells) {
            val scenario = "${cell["implementation"]}-${cell["pattern"]}"
            for ((metric, field) in RESULT_METRICS) {
                val value = cell[field] as? Number ?: continue
                lines.add(
                    "RESULT,current,%s,local,%s,%s,%.3f".format(
                        scenario, cell["payload_size"], metric, value.toDouble(),
                    ),
                )
            }
        }
        val report = lines.joinToString("\n") + "\n"
        Files.writeString(
            Path.of(options.output, options.reportFile), report, StandardCharsets.UTF_8,
        )
        Files.writeString(Path.of(options.output, "report.txt"), report, StandardCharsets.UTF_8)

        if (failures.isNotEmpty() || contaminated.isNotEmpty() ||
            drainObservations.isNotEmpty()
        ) {
            val summary = mutableListOf<String>()
            if (drainObservations.isNotEmpty()) {
                summary.add("## Drain (FB-008)")
                drainObservations.forEach { summary.add("- $it") }
                summary.add("")
            }
            if (contaminated.isNotEmpty()) {
                summary.add("## Contaminated (excluded from tables and judgement)")
                contaminated.forEach { summary.add("- $it") }
                summary.add("")
            }
            summary.add("## Failures")
            failures.forEach { summary.add("- $it") }
            Files.writeString(
                Path.of(options.output, "failures.txt"),
                summary.joinToString("\n") + "\n",
                StandardCharsets.UTF_8,
            )
            System.err.println(summary.joinToString("\n"))
        }
    }

    private fun collectMetadata(): Map<String, Any?> {
        val metadata = LinkedHashMap<String, Any?>()
        metadata["diagnosticsSchema"] = "with-grpc-cell-v1"
        metadata["language"] = "kotlin"
        metadata["kotlinVersion"] = KotlinVersion.CURRENT.toString()
        metadata["javaVersion"] = System.getProperty("java.version")
        metadata["javaVendor"] = System.getProperty("java.vendor")
        metadata["javaVmName"] = System.getProperty("java.vm.name")
        metadata["grpcJavaVersion"] = "1.72.0"
        metadata["grpcKotlinVersion"] = "1.4.1"
        metadata["grpcStub"] = "grpc-kotlin coroutine stub (BenchServiceCoroutineStub)"
        metadata["coroutinesVersion"] = "1.9.0"
        metadata["coroutineDispatcher"] = "Dispatchers.Default, CoroutineStart.UNDISPATCHED"
        metadata["protobufJavaVersion"] = "4.30.2"
        metadata["grpcServerConfiguration"] =
            stats.info(options.grpcStatsUrl).replace("\"", "'")
        // The kotlin row owns the client only. The three server processes are the java
        // row's binaries on the kotlin port band; what differs between the two rows is
        // the client-facing API.
        metadata["serverProcesses"] = "java row binaries (bench-grpc-server," +
            " bench-zlink-raw-server, bench-zlink-framework-server) on the kotlin band"
        metadata["zlinkBindingVersion"] = "0.17.0"
        metadata["zlinkBindingNote"] =
            "bindings/kotlin has no native binding of its own; it is systems.zlink:zlink" +
                " used from kotlin"
        metadata["frameworkModule"] = "zlink-framework-kotlin 0.10.0"
        metadata["frameworkHost"] = "zlink-framework-spring-boot-starter 0.10.0"
        metadata["logical_cores"] = ClientResources.LOGICAL_CORES
        metadata["client_saturation_metric"] = ClientResources.CLIENT_SATURATION_METRIC
        // Declared per driver, not per run: 1 for the serial and window drivers, the send
        // concurrency for the send driver. The per-cell value is authoritative.
        metadata["client_parallelism_ceiling"] =
            "1 (request drivers) / ${options.sendConcurrency} (send driver)"
        metadata["cpu"] = readCpuModel()
        metadata["kernel"] = System.getProperty("os.version")
        metadata["commit"] = readCommit()
        metadata["rawSocket"] = options.rawSocket
        metadata["warmupSeconds"] = options.warmupSeconds
        metadata["warmupSegmentSeconds"] = options.warmupSegmentSeconds
        metadata["durationSeconds"] = options.durationSeconds
        metadata["requestWindow"] = options.requestWindow
        metadata["sendConcurrency"] = options.sendConcurrency
        metadata["requestTimeoutMs"] = options.requestTimeoutMs
        metadata["channel"] = BenchContract.CHANNEL_NAME
        metadata["runId"] = options.runId
        metadata["startedAt"] = ZonedDateTime.now().toString()
        metadata["loadavg1"] = readLoadavg()
        metadata["contaminatedCells"] = contaminated
        return metadata
    }

    private fun readCpuModel(): String {
        return try {
            Files.readAllLines(Path.of("/proc/cpuinfo"))
                .firstOrNull { it.startsWith("model name") }
                ?.substringAfter(':')
                ?.trim()
                ?: "unknown"
        } catch (error: Exception) {
            "unknown"
        }
    }

    private fun readCommit(): String {
        return try {
            val process = ProcessBuilder("git", "rev-parse", "HEAD")
                .redirectErrorStream(true)
                .start()
            val output = String(process.inputStream.readAllBytes(), StandardCharsets.UTF_8).trim()
            process.waitFor()
            output.ifEmpty { "unknown" }
        } catch (error: Exception) {
            "unknown"
        }
    }

    private fun readLoadavg(): Double {
        return try {
            Files.readString(Path.of("/proc/loadavg")).trim().split(Regex("\\s+"))[0].toDouble()
        } catch (error: Exception) {
            -1.0
        }
    }

    companion object {
        const val GRPC = "grpc-kotlin"
        const val RAW = "zlink-kotlin"
        const val FRAMEWORK = "zlink-framework-kotlin"

        private val PATTERNS = listOf("request-serial", "request-window", "send-saturation")

        private val RESULT_METRICS = linkedMapOf(
            "throughput" to "throughput_per_second",
            "bandwidth" to "bandwidth_mb_s",
            "latency" to "latency_mean_ms",
            "latency_p95" to "latency_p95_ms",
            "latency_p99" to "latency_p99_ms",
            "client_cpu_percent" to "client_cpu_percent",
            "client_memory_mb" to "client_memory_mb",
            "server_cpu_percent" to "server_cpu_percent",
            "server_memory_mb" to "server_memory_mb",
        )
    }
}

fun main(args: Array<String>) {
    val status = BenchKotlinClient(BenchOptions(args)).run()
    System.exit(status)
}
