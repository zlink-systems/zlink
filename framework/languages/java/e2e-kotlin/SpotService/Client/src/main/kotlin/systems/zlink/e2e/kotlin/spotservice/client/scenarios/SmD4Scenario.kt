package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.expectFailure

internal object SmD4Scenario {
    suspend fun run() {
        val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
        try {
            connector.connect().await()
            val profile = Contracts.ActorProfile("Multi Bind", 9, listOf("multi", "bind"))
            val bound = connector
                .request(Contracts.MultiBindReq("actor-sm-d4-x", "actor-sm-d4-y", profile))
                .awaitReply<Contracts.MultiBindRes>()
            ensure(bound.boundCount == 2, "SM-D4 expected two bound actors")

            val x = connector
                .request(Contracts.ActorEchoReq("to-x", 10, profile))
                .metadata("actor-id", "actor-sm-d4-x")
                .awaitReply<Contracts.ActorEchoRes>()
            val y = connector
                .request(Contracts.ActorEchoReq("to-y", 11, profile))
                .metadata("actor-id", "actor-sm-d4-y")
                .awaitReply<Contracts.ActorEchoRes>()
            ensure(x.actorId == "actor-sm-d4-x" && x.value == "entry:to-x", "SM-D4 x relay mismatch")
            ensure(y.actorId == "actor-sm-d4-y" && y.value == "entry:to-y", "SM-D4 y relay mismatch")

            val xPush = coroutineScope {
                val pushed = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Contracts.ActorPushNotify>()
                        .where { it.payload().actorId == "actor-sm-d4-x" }
                        .await()
                }
                connector
                    .request(Contracts.ActorEchoReq("push-x", 12, profile))
                    .metadata("actor-id", "actor-sm-d4-x")
                    .awaitReply<Contracts.ActorEchoRes>()
                pushed.await().payload()
            }
            ensure(xPush.actorId == "actor-sm-d4-x" && xPush.value == "push:push-x", "SM-D4 x push mismatch")

            val yPush = coroutineScope {
                val pushed = async(start = CoroutineStart.UNDISPATCHED) {
                    connector.waitFor<Contracts.ActorPushNotify>()
                        .where { it.payload().actorId == "actor-sm-d4-y" }
                        .await()
                }
                connector
                    .request(Contracts.ActorEchoReq("push-y", 13, profile))
                    .metadata("actor-id", "actor-sm-d4-y")
                    .awaitReply<Contracts.ActorEchoRes>()
                pushed.await().payload()
            }
            ensure(yPush.actorId == "actor-sm-d4-y" && yPush.value == "push:push-y", "SM-D4 y push mismatch")

            expectFailure {
                connector
                    .request(Contracts.ActorEchoReq("missing-actor-id", 14, profile))
                    .timeout(Duration.ofSeconds(2))
                    .awaitReply<Contracts.ActorEchoRes>()
            }

            println("scenario SM-D4 passed")
        } finally {
            try {
                connector.close().await()
            } catch (_: Exception) {
            }
        }
    }
}
