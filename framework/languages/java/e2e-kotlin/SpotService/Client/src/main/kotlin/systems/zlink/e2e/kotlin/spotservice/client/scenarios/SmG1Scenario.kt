package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually
import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure
import systems.zlink.e2e.kotlin.spotservice.client.support.sleep
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector

internal object SmG1Scenario {
    suspend fun run(spots: SpotHttpDriver) {
        val actorId = "actor-sm-g1-" + UUID.randomUUID().toString().replace("-", "")
        val profile = Contracts.ActorProfile("Crash Recovery", 1, listOf("sm-g1"))
        val crashedConnector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            crashedConnector.connect().await()
            authenticateJoinAndEcho(crashedConnector, actorId, profile, "before-crash", 1)
            signalFile("e2e.sm.g1.ready.file")
            waitForSignalFile("e2e.sm.g1.crashed.file")

            expectFailure {
                crashedConnector
                    .request(Contracts.ActorEchoReq("during-crash", 2, profile))
                    .metadata("actor-id", actorId)
                    .timeout(Duration.ofSeconds(2))
                    .awaitReply<Contracts.ActorEchoRes>()
            }

            val survivor = eventually {
                spots.requestState("room-b", "sm-g1-survivor")
            }
            ensure(survivor.nodeRid == "play-b", "SM-G1 play-b survivor request node mismatch")
            signalFile("e2e.sm.g1.failed.file")
            waitForSignalFile("e2e.sm.g1.restarted.file")
        } finally {
            closeQuietly(crashedConnector)
        }

        val recoveredConnector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            recoveredConnector.connect().await()
            authenticateJoinAndEcho(recoveredConnector, actorId, profile, "after-restart", 3)
        } finally {
            closeQuietly(recoveredConnector)
        }

        println("scenario SM-G1 passed")
    }

    private suspend fun authenticateJoinAndEcho(
        connector: ZLinkKotlinStreamConnector,
        actorId: String,
        profile: Contracts.ActorProfile,
        value: String,
        requestSeq: Int
    ) {
        val auth = connector
            .request(Contracts.ActorAuthReq(actorId, profile))
            .timeout(Duration.ofSeconds(15))
            .awaitReply<Contracts.ActorAuthRes>()
        ensure(auth.actorId == actorId, "SM-G1 auth actor mismatch")

        val joined = connector
            .request(Contracts.ActorJoinReq("room-a", profile, profile.tags))
            .metadata("actor-id", actorId)
            .timeout(Duration.ofSeconds(15))
            .awaitReply<Contracts.ActorJoinRes>()
        ensure(joined.spotRid == "room-a", "SM-G1 joined spot mismatch")

        val echo = connector
            .request(Contracts.ActorEchoReq(value, requestSeq, profile))
            .metadata("actor-id", actorId)
            .timeout(Duration.ofSeconds(15))
            .awaitReply<Contracts.ActorEchoRes>()
        ensure(echo.spotRid == "room-a", "SM-G1 actor spot mismatch")
        ensure(echo.value == "user:$value", "SM-G1 actor echo mismatch")
    }

    private fun signalFile(envName: String) {
        val path = Env.get(envName)
        ensure(path.isNotBlank(), "$envName is required")
        Files.writeString(Path.of(path), "ready\n")
    }

    private suspend fun waitForSignalFile(envName: String) {
        val path = Env.get(envName)
        ensure(path.isNotBlank(), "$envName is required")
        val signal = Path.of(path)
        val deadline = System.nanoTime() + Duration.ofSeconds(60).toNanos()
        while (System.nanoTime() < deadline) {
            if (Files.exists(signal)) {
                return
            }
            sleep(100)
        }
        throw IllegalStateException("timed out waiting for signal file $path")
    }

    private suspend fun closeQuietly(connector: ZLinkKotlinStreamConnector) {
        try {
            connector.close().await()
        } catch (_: Exception) {
        }
    }
}
