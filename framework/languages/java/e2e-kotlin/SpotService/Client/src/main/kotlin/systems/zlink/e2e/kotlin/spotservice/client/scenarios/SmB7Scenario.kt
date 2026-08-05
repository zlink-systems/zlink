package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmB7Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        context.requestOrderedUserEchoes()

        ensure(context.userReply2.requestSeq == 3, "SM-B7 second packet request sequence mismatch")
        ensure(context.userPush2.requestSeq == 3, "SM-B7 second push request sequence mismatch")
        ensure(context.userReply3.requestSeq == 4, "SM-B7 third packet request sequence mismatch")
        ensure(context.userPush3.requestSeq == 4, "SM-B7 third push request sequence mismatch")
        ensure(
            context.userReply1.handlerSeq < context.userReply2.handlerSeq &&
                context.userReply2.handlerSeq < context.userReply3.handlerSeq,
            "SM-B7 actor packet handler sequence was not preserved",
        )
        ensure(
            context.userPush1.handlerSeq < context.userPush2.handlerSeq &&
                context.userPush2.handlerSeq < context.userPush3.handlerSeq,
            "SM-B7 actor push sequence was not preserved",
        )

        println("scenario SM-B7 passed")
    }
}
