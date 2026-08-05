package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure

internal object SmB3Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        ensure(context.auth.displayName == context.profile.displayName, "SM-B3 create profile display name mismatch")
        ensure(context.auth.level == context.profile.level, "SM-B3 create profile level mismatch")
        ensure(context.auth.tags == context.profile.tags, "SM-B3 create profile tags mismatch")

        ensure(context.entryReply.requestSeq == 1, "SM-B3 entry request sequence mismatch")
        ensure(context.entryReply.displayName == context.profile.displayName, "SM-B3 entry profile display name mismatch")
        ensure(context.entryReply.level == context.profile.level, "SM-B3 entry profile level mismatch")
        ensure(context.entryReply.tags == context.profile.tags, "SM-B3 entry profile tags mismatch")

        ensure(context.joined.tags == context.profile.tags, "SM-B3 join payload tags mismatch")
        ensure(context.joined.displayName == context.profile.displayName, "SM-B3 join payload display name mismatch")
        ensure(context.joined.level == context.profile.level, "SM-B3 join payload level mismatch")

        ensure(context.userReply1.requestSeq == 2, "SM-B3 user request sequence mismatch")
        ensure(context.userReply1.displayName == context.profile.displayName, "SM-B3 user profile display name mismatch")
        ensure(context.userReply1.level == context.profile.level, "SM-B3 user profile level mismatch")
        ensure(context.userReply1.tags == context.profile.tags, "SM-B3 user profile tags mismatch")

        println("scenario SM-B3 passed")
    }
}
