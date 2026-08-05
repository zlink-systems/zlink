package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.util.UUID
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.postJson

internal object SmB6Scenario {
    suspend fun run() {
        val suffix = UUID.randomUUID().toString().replace("-", "")
        val leaveActorId = "actor-sm-b6-left-$suffix"
        val disconnectActorId = "actor-sm-b6-disconnected-$suffix"
        val profile = Contracts.ActorProfile("Leave", 6, listOf("leave"))

        val leaveClient = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            leaveClient.connect().await()
            val auth = leaveClient
                .request(Contracts.ActorAuthReq(leaveActorId, profile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == leaveActorId, "SM-B6 leave auth actor mismatch")

            val joined = leaveClient
                .request(Contracts.ActorJoinReq("actor-room-a", profile, profile.tags))
                .metadata("actor-id", leaveActorId)
                .awaitReply<Contracts.ActorJoinRes>()
            ensure(joined.actorId == leaveActorId, "SM-B6 leave join actor mismatch")

            val left = leaveClient
                .request(Contracts.LeaveActorReq(leaveActorId))
                .metadata("actor-id", leaveActorId)
                .awaitReply<Contracts.LeaveActorRes>()
            ensure(left.accepted && left.actorId == leaveActorId, "SM-B6 leave reply mismatch")
        } finally {
            try {
                leaveClient.close().await()
            } catch (_: Exception) {
            }
        }

        val leaveEvidence = postJson(
            Env.get("e2e.http.session.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf("ActorUserLeft|session-a|actor-room-a|$leaveActorId"),
                10_000,
            ),
            Contracts.EvidenceSnapshot::class.java,
        )
        ensure(
            leaveEvidence.entries.none {
                it.marker == "ActorUserDisconnected" &&
                    it.spotRid == "actor-room-a" &&
                    it.value == leaveActorId
            },
            "SM-B6 explicit leave emitted disconnect evidence",
        )

        val disconnectClient = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            val disconnectProfile = Contracts.ActorProfile("Disconnect", 6, listOf("disconnect"))
            disconnectClient.connect().await()
            val auth = disconnectClient
                .request(Contracts.ActorAuthReq(disconnectActorId, disconnectProfile))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(auth.actorId == disconnectActorId, "SM-B6 disconnect auth actor mismatch")
            val joined = disconnectClient
                .request(Contracts.ActorJoinReq("actor-room-a", disconnectProfile, disconnectProfile.tags))
                .metadata("actor-id", disconnectActorId)
                .awaitReply<Contracts.ActorJoinRes>()
            ensure(joined.actorId == disconnectActorId, "SM-B6 disconnect join actor mismatch")
            disconnectClient.close().await()
        } finally {
            try {
                disconnectClient.close().await()
            } catch (_: Exception) {
            }
        }

        val disconnectEvidence = postJson(
            Env.get("e2e.http.session.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf("ActorUserDisconnected|session-a|actor-room-a|$disconnectActorId"),
                10_000,
            ),
            Contracts.EvidenceSnapshot::class.java,
        )
        ensure(
            disconnectEvidence.entries.none {
                it.marker == "ActorUserLeft" &&
                    it.value == disconnectActorId
            },
            "SM-B6 disconnect emitted leave evidence",
        )

        println("scenario SM-B6 passed")
    }
}
