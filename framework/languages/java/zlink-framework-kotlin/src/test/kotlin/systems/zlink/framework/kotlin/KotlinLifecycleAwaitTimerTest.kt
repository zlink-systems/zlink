package systems.zlink.framework.kotlin

import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import org.junit.jupiter.params.ParameterizedTest
import org.junit.jupiter.params.provider.CsvSource
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest.Actor
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest.COMPLETION_SECONDS
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest.PERIOD
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest.Phase
import systems.zlink.framework.runtime.spots.ZLinkLifecycleAwaitTimerTest.TIMER_OPTIONS
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkTimerTick

class KotlinLifecycleAwaitTimerTest {
    @ParameterizedTest
    @CsvSource("false,false", "true,false", "false,true", "true,true")
    fun joinedSuspendAwaitRetainsGateUnlessExplicitlyYielded(
        explicitMode: Boolean,
        yield: Boolean,
    ) {
        ZLinkLifecycleAwaitTimerTest.runScenario(Room::class.java, explicitMode, yield)
    }

    class Room(override val context: ZLinkSpotContext) : ZLinkSuspendingSpot<Actor>() {
        override suspend fun onInitializeSuspending() {
            context.addTimer<Tick>("probe", PERIOD, TIMER_OPTIONS).await()
        }

        override suspend fun onActorJoinSuspending(
            actorId: String,
            request: ZLinkMessage,
        ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.accept()

        override suspend fun onJoinedActorSuspending(actor: Actor) {
            val probe = ZLinkLifecycleAwaitTimerTest.probe
            probe.phase.set(Phase.WAITING)
            val worker =
                context
                    .runIoWorker {
                        probe.entered.countDown()
                        check(probe.release.await(COMPLETION_SECONDS, TimeUnit.SECONDS))
                        CompletableFuture.completedFuture(true)
                    }
                    .kotlin()
            if (probe.yield) worker.yield() else worker.await()
            probe.phase.set(Phase.COMPLETED)
            probe.joined.countDown()
        }

        override suspend fun onLeaveActorSuspending(actor: Actor) {}
    }

    class Tick : ZLinkSuspendingSpotTimerHandler<Room> {
        override suspend fun handle(spot: Room, tick: ZLinkTimerTick) {
            val probe = ZLinkLifecycleAwaitTimerTest.probe
            if (probe.phase.get() == Phase.WAITING) probe.overlap.countDown()
            if (probe.phase.get() == Phase.COMPLETED) probe.after.countDown()
        }
    }
}
