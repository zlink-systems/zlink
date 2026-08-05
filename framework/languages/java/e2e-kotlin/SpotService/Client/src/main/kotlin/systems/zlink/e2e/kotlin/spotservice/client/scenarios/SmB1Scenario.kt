package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmB1Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        context.connectAndAuthenticate()
        ensure(context.auth.actorId == context.actorId, "SM-B1 auth actor mismatch")
        ensure(context.auth.boundCount == 1, "SM-B1 bound actor count mismatch")

        context.requestEntryEcho()
        ensure(
            context.entryReply.value == "entry:entry-echo",
            "SM-B1 entry actor request mismatch: ${context.entryReply.value}"
        )

        context.joinRoom()
        ensure(context.joined.spotRid == "actor-room-a", "SM-B1 joined spot mismatch")

        context.requestFirstUserEchoAndCheckUnboundIsolation()
        ensure(context.userReply1.spotRid == "actor-room-a", "SM-B1 user actor spot mismatch")
        ensure(
            context.userReply1.value == "user:user-echo-1",
            "SM-B1 user actor request mismatch: ${context.userReply1.value}"
        )

        println("scenario SM-B1 passed")
    }
}
