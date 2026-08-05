package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import java.util.UUID
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmRemoteActorSessionScenario {
    suspend fun run() {
        val actorId = "actor-sm-remote-" + UUID.randomUUID().toString().replace("-", "")
        val connector = createStreamConnector(Env.get("e2e.stream.b.endpoint"))
        val profile = Contracts.ActorProfile("Remote Player", 24, listOf("remote", "relay"))
        try {
            connector.connect().await()
            val auth = connector
                .request(Contracts.ActorAuthReq(actorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == actorId, "SM-B2 remote auth actor mismatch")
            ensure(auth.nodeRid == "play-b", "SM-B2 remote actor was not created on play-b")

            val joined = connector
                .request(Contracts.ActorJoinReq("room-a", profile, profile.tags))
                .metadata("actor-id", actorId)
                .timeout(Duration.ofSeconds(15))
                .awaitReply<Contracts.ActorJoinRes>()
            ensure(joined.actorId == actorId, "SM-B2 remote join actor mismatch")
            ensure(joined.spotRid == "room-a", "SM-B2 remote join spot mismatch")
            ensure(joined.nodeRid == "play-a", "SM-B2 remote join did not cross to play-a")

            val (userReply, push) = coroutineScope {
                val userPush = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Contracts.ActorPushNotify>().await()
                }
                val userReply = connector
                    .request(Contracts.ActorEchoReq("remote-user-echo", 42, profile))
                    .metadata("actor-id", actorId)
                    .timeout(Duration.ofSeconds(15))
                    .awaitReply<Contracts.ActorEchoRes>()
                userReply to userPush.await().payload()
            }
            ensure(userReply.value == "user:remote-user-echo", "SM-B4 remote actor reply mismatch")
            ensure(userReply.spotRid == "room-a", "SM-B4 remote actor reply spot mismatch")
            ensure(userReply.nodeRid == "play-a", "SM-B4 remote actor reply node mismatch")
            ensure(push.value == "push:remote-user-echo", "SM-D2 remote bound-session push mismatch")
            ensure(push.actorId == actorId, "SM-D2 remote bound-session push actor mismatch")

            waitForSessionEvidence(
                Env.get("e2e.http.b.endpoint"),
                "ActorCreated|play-b|entry|$actorId",
            )
            waitForSessionEvidence(
                Env.get("e2e.http.a.endpoint"),
                "ActorUserJoined|play-a|room-a|$actorId",
                "ActorUserRequest|play-a|room-a|$actorId/remote-user-echo",
            )

            println("scenario SM-B2 passed")
            println("scenario SM-B4 passed")
            println("scenario SM-D2 passed")
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }
    }

    private fun waitForSessionEvidence(endpoint: String, vararg expected: String) {
        postJson(
            endpoint,
            "/evidence/wait",
            Contracts.EvidenceWaitReq(expected.toList(), 10_000),
            Contracts.EvidenceSnapshot::class.java,
        )
    }
}
