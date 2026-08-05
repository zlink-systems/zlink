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
