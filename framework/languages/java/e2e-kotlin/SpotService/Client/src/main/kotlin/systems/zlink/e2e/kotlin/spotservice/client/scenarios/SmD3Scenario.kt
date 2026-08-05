package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmD3Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        ensure(context.entryReply.spotRid == "entry", "SM-D3 entry bind spot mismatch")
        ensure(context.entryPush.spotRid == "entry", "SM-D3 entry push spot mismatch")
        ensure(context.entryReply.actorId == context.auth.actorId, "SM-D3 entry bind actor mismatch")
        ensure(context.entryPush.actorId == context.auth.actorId, "SM-D3 entry push actor mismatch")

        ensure(context.joined.spotRid == "actor-room-a", "SM-D3 user bind spot mismatch")
        ensure(context.userReply1.spotRid == "actor-room-a", "SM-D3 user relay spot mismatch")
        ensure(context.userPush1.spotRid == "actor-room-a", "SM-D3 user push spot mismatch")
        ensure(context.userReply1.actorId == context.auth.actorId, "SM-D3 user relay actor mismatch")
        ensure(context.userPush1.actorId == context.auth.actorId, "SM-D3 user push actor mismatch")

        println("scenario SM-D3 passed")
    }
}
