package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import java.net.Socket
import java.net.URI
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ReplacementSupport
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonD1FailureRecoveryScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        val serviceBUri = URI.create(options.filteredServiceHttp)
        evidence.post("${options.filteredServiceHttp}/shutdown")
        waitForPort(serviceBUri, shouldBeOpen = false, "MON-D1 expected service-b to stop")

        val restarted = startServiceB()
        try {
            waitForPort(serviceBUri, shouldBeOpen = true, "MON-D1 expected service-b to restart")
            val reply = evidence.post("${options.triggerHttp}/profile/request/service-b")
            ScenarioAssert.ensure(
                reply.contains("\"providerRid\":\"svc-b\"") &&
                    reply.contains("\"value\":\"work:mon-d1-request\""),
                "MON-D1 restarted service-b did not handle request: $reply",
            )
            evidence.waitForEvent(
                options.filteredServiceHttp,
                "socket",
                Contracts.CHANNEL,
                setOf("CONNECTION_READY"),
            )
            evidence.waitForEvent(
                options.serviceHttp,
                "location",
                Contracts.LOCATION_SOURCE,
                setOf("TOPOLOGY_CHANGED"),
            )
        } finally {
            evidence.postBestEffort("${options.filteredServiceHttp}/shutdown")
            restarted.waitFor(10, TimeUnit.SECONDS)
            if (restarted.isAlive) {
                restarted.destroyForcibly()
                restarted.waitFor(5, TimeUnit.SECONDS)
            }
        }

        println("scenario MON-D1 passed")
    }

    fun runUnknownMesh() {
        val query = evidence.postRaw("${options.serviceHttp}/runtime/unknown-mesh")
        val observe = evidence.postRaw("${options.serviceHttp}/runtime/unknown-observe")
        ScenarioAssert.ensure(query.first in 400..499 && observe.first in 400..499, "MON-D1A invalid public MeshName was accepted: query=$query observe=$observe")
        println("scenario MON-D1A passed")
    }

    fun runRepeatedReplacement() {
        val replacement = ReplacementSupport(options)
        evidence.post("${options.serviceHttp}/runtime/observer/start")
        var lastSequence = -1L
        var current: Process? = null
        repeat(3) { cycle ->
            val stopped = evidence.postRaw("${options.filteredServiceHttp}/crash")
            ScenarioAssert.ensure(stopped.first in 200..299, "MON-D1B cycle ${cycle + 1} crash control failed: $stopped")
            replacement.waitForPort(false, "MON-D1B cycle ${cycle + 1} old process remained reachable")
            replacement.waitForMesh(false, "MON-D1B cycle ${cycle + 1} old mesh remained bound")
            awaitPeerRemoved("MON-D1B cycle ${cycle + 1}")
            current = replacement.startFiltered()
            try {
                replacement.waitForPort(true, "MON-D1B cycle ${cycle + 1} replacement did not start")
                replacement.waitForMesh(true, "MON-D1B cycle ${cycle + 1} replacement mesh did not start")
                awaitReady("MON-D1B cycle ${cycle + 1}")
                val observer = awaitObserver()
                ScenarioAssert.ensure(observer.path("latestReady").asBoolean(), "MON-D1B observer did not converge to ready")
                ScenarioAssert.ensure(observer.path("latestSequence").asLong() > lastSequence, "MON-D1B observer sequence did not advance")
                lastSequence = observer.path("latestSequence").asLong()
                val reply = evidence.post("${options.triggerHttp}/profile/request/service-b?value=mon-d1b-$cycle")
                ScenarioAssert.ensure(reply.contains("\"providerRid\":\"svc-b\""), "MON-D1B replacement request failed: $reply")
            } finally {
                if (cycle == 2) {
                    evidence.postBestEffort("${options.filteredServiceHttp}/shutdown")
                    replacement.stop(current)
                }
            }
        }
        println("scenario MON-D1B passed")
    }

    private fun awaitReady(scenario: String) {
        repeat(200) {
            val status = evidence.json("${options.serviceHttp}/runtime/snapshot")
            if (status.path("ready").asBoolean() && status.path("readyPeerCount").asInt() > 0) return
            ScenarioAssert.sleep(100)
        }
        throw IllegalStateException("$scenario replacement did not become READY")
    }

    private fun awaitPeerRemoved(scenario: String) {
        repeat(200) {
            val status = evidence.json("${options.serviceHttp}/runtime/snapshot")
            val topology = evidence.json("${options.serviceHttp}/runtime/topology")
            val stale = topology.any { row ->
                row.path("nodeRid").asText().contains("svc-b")
            }
            if (status.path("readyPeerCount").asInt() == 0 && !stale) return
            ScenarioAssert.sleep(100)
        }
        val lastSnapshot = evidence.json("${options.serviceHttp}/runtime/snapshot")
        val lastTopology = evidence.json("${options.serviceHttp}/runtime/topology")
        throw IllegalStateException(
            "$scenario stale svc-b peer did not leave public topology: " +
                "snapshot=$lastSnapshot topology=$lastTopology",
        )
    }

    private fun awaitObserver() = run {
        repeat(200) {
            val status = evidence.json("${options.serviceHttp}/runtime/observer/status")
            if (status.path("latestSequence").asLong() > 0) return@run status
            ScenarioAssert.sleep(100)
        }
        throw IllegalStateException("MON-D1B observer did not publish a status")
    }

    private fun startServiceB(): Process {
        val stdout = Path.of(options.logDir, "filtered-service-restart.stdout.log").toFile()
        val stderr = Path.of(options.logDir, "filtered-service-restart.stderr.log").toFile()
        val config = Path.of(options.logDir, "filtered-service-restart.properties")
        Files.writeString(
            config,
            listOf(
                "e2e.rid=svc-b",
                "e2e.redis.location.endpoint=${Env.get("e2e.redis.location.endpoint")}",
                "e2e.location.key.prefix=${Env.get("e2e.location.key.prefix")}",
                "e2e.api.endpoint=${options.filteredApiEndpoint}",
                "e2e.http.endpoint=${options.filteredServiceHttp}",
                "e2e.mesh.endpoint=${options.filteredMeshEndpoint}",
                "e2e.log.dir=${options.logDir}",
            ).joinToString("\n") + "\n",
        )
        val process = ProcessBuilder(options.filteredServiceBin, "--e2e-config", config.toString())
            .redirectOutput(stdout)
            .redirectError(stderr)
        return process.start()
    }

    private fun waitForPort(uri: URI, shouldBeOpen: Boolean, failureMessage: String) {
        repeat(100) {
            if (canConnect(uri) == shouldBeOpen) {
                return
            }
            ScenarioAssert.sleep(100)
        }
        throw IllegalStateException(failureMessage)
    }

    private fun canConnect(uri: URI): Boolean {
        return try {
            Socket(uri.host, uri.port).use { true }
        } catch (_: Exception) {
            false
        }
    }
}
