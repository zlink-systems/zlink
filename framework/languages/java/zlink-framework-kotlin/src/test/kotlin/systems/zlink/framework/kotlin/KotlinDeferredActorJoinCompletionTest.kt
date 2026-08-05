package systems.zlink.framework.kotlin

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.messaging.ZLinkMessage

class KotlinDeferredActorJoinCompletionTest {
    @Test
    fun suspendingActorReceivesTheDurableAcceptedIdentityAndGeneration() {
        val actor = RecordingSuspendingActor()
        val operation = ZLinkActorJoinOperationId(41, 73)
        val completion = ZLinkActorJoinCompletion.Accepted(
            operation,
            ActorRef("actor-a", 17, "game", RoutingId.from("target-node")),
            ZLinkMessage.of("accepted"),
        )

        actor.onJoinCompleted(completion).toCompletableFuture().join()

        assertEquals(operation, actor.received?.operationId())
        assertEquals(17, actor.received?.actor()?.objectGeneration())
        assertEquals("accepted", actor.received?.reply()?.decode(String::class.java))
    }

    private class RecordingSuspendingActor : ZLinkSuspendingActor() {
        override val context: ZLinkActorContext
            get() = error("context is not used by this callback bridge test")

        var received: ZLinkActorJoinCompletion.Accepted? = null

        override suspend fun onJoinCompletedSuspending(
            completion: ZLinkActorJoinCompletion,
        ) {
            received = completion as ZLinkActorJoinCompletion.Accepted
        }
    }
}
