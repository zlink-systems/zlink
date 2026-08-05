package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.util.UUID
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmD12Scenario {
    suspend fun run() {
        val actorId = "actor-sm-d12-" + UUID.randomUUID().toString().replace("-", "")
        val profile = Contracts.ActorProfile("Transfer Player", 12, listOf("transfer"))
        val first = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        val second = createStreamConnector(Env.get("e2e.stream.b.endpoint"))
        try {
            first.connect().await()
            val firstAuth = first
                .request(Contracts.ActorRemoteAuthReq(actorId, profile, "play-a"))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(firstAuth.actorId == actorId, "SM-D12 first auth actor mismatch")
            ensure(firstAuth.nodeRid == "play-a", "SM-D12 first auth node mismatch")

            val firstReply = first
                .request(Contracts.ActorEchoReq("before-transfer", 1, profile))
                .metadata("actor-id", actorId)
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(firstReply.nodeRid == "play-a", "SM-D12 first actor node mismatch")
            ensure(firstReply.handlerSeq > 0, "SM-D12 initial actor state was not updated")
            first.close().await()

            second.connect().await()
            val secondAuth = second
                .request(Contracts.ActorRemoteAuthReq(actorId, profile, "play-a"))
                .awaitReply<Contracts.ActorAuthRes>()
            ensure(secondAuth.actorId == actorId, "SM-D12 second auth actor mismatch")
            ensure(secondAuth.nodeRid == "play-a", "SM-D12 second auth node mismatch")

            val (resumed, notify) = coroutineScope {
                val push = async(start = CoroutineStart.UNDISPATCHED) {
                    second.waitFor<Contracts.ActorPushNotify>().await()
                }
                val resumed = second
                    .request(Contracts.ActorEchoReq("after-transfer", 2, profile))
                    .metadata("actor-id", actorId)
                    .awaitReply<Contracts.ActorEchoRes>()
                resumed to push.await().payload()
            }
            ensure(resumed.actorId == actorId, "SM-D12 resumed actor mismatch")
            ensure(resumed.nodeRid == "play-a", "SM-D12 resumed node mismatch")
            val expectedResumedSequence = firstReply.handlerSeq + 1
            ensure(
                resumed.handlerSeq == expectedResumedSequence,
                "SM-D12 actor state was not preserved across session servers"
            )
            ensure(notify.actorId == actorId, "SM-D12 resumed push actor mismatch")
            ensure(notify.value == "push:after-transfer", "SM-D12 resumed push value mismatch")
            ensure(notify.handlerSeq == expectedResumedSequence, "SM-D12 resumed push state mismatch")
            println("scenario SM-D12 passed")
        } finally {
            closeQuietly(first)
            closeQuietly(second)
        }
    }

    private suspend fun closeQuietly(connector: ZLinkKotlinStreamConnector) {
        try {
            connector.close().await()
        } catch (_: Exception) {
        }
    }
}
