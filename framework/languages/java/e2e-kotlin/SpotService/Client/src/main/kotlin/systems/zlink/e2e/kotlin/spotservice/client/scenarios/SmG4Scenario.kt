package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.framework.kotlin.*

import java.util.UUID
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.coroutineScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.client.support.createStreamConnector
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector

internal object SmG4Scenario {
    suspend fun run() {
        val connectors = mutableListOf<ZLinkKotlinStreamConnector>()
        val actorIds = mutableListOf<String>()
        val values = mutableListOf<String>()
        try {
            repeat(8) { index ->
                val actorId = "actor-sm-g4-$index-" + UUID.randomUUID().toString().replace("-", "")
                val profile = Contracts.ActorProfile("Bound Load $index", 14, listOf("load", "session-$index"))
                val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
                connectors += connector
                actorIds += actorId
                values += "push-$index"
                connector.connect().await()
                val auth = connector
                    .request(Contracts.ActorAuthReq(actorId, profile))
                    .awaitReply<Contracts.ActorAuthRes>()
                ensure(auth.actorId == actorId, "SM-G4 auth actor mismatch")
            }

            val (pushes, replies) = coroutineScope {
                val pushes = connectors.map { connector ->
                    async(start = CoroutineStart.UNDISPATCHED) {
                        connector.waitFor<Contracts.ActorPushNotify>().await()
                    }
                }
                connectors.mapIndexed { index, connector ->
                    val value = values[index]
                    val profile = Contracts.ActorProfile("Bound Load Request $index", 14, listOf("load"))
                    async {
                        connector.request(Contracts.ActorEchoReq(value, 14, profile))
                            .awaitReply<Contracts.ActorEchoRes>()
                    }
                }.awaitAll().let { pushes to it }
            }

            connectors.forEachIndexed { index, connector ->
                val actorId = actorIds[index]
                val value = values[index]
                val reply = replies[index]
                val notify = pushes[index].await().payload()
                ensure(reply.actorId == actorId, "SM-G4 push reply actor mismatch")
                ensure(reply.value == "entry:$value", "SM-G4 push reply value mismatch")
                ensure(notify.actorId == actorId, "SM-G4 push notify actor mismatch")
                ensure(notify.value == "push:$value", "SM-G4 push notify value mismatch")
            }

            println("scenario SM-G4 passed")
        } finally {
            connectors.forEach { connector ->
                try {
                    connector.close().await()
                } catch (_: Exception) {
                }
            }
        }
    }
}
