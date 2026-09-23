package systems.zlink.samples.kotlin.bingo.server.matchmaking

import java.time.Duration
import java.time.Instant
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicInteger
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext

class BingoMatchmaker(private val instanceContext: ZLinkInstanceSpotContext) : ZLinkInstanceSpot {
    private val inFlight = AtomicInteger()
    @Volatile private var lastActivity: Instant = Instant.now()

    override fun context(): ZLinkInstanceSpotContext = instanceContext

    override fun onInitialize(): CompletionStage<Void> =
        instanceContext
            .addTimer<BingoMatchmakerIdleTimerHandler>(
                "matchmaker-idle-close",
                IdleCheckPeriod,
                systems.zlink.framework.spots.ZLinkTimerOptions(
                    systems.zlink.framework.spots.ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
                    1,
                    false,
                ),
            )
            .thenApply { null }

    fun beginRequest() {
        inFlight.incrementAndGet()
        lastActivity = Instant.now()
    }

    fun endRequest() {
        lastActivity = Instant.now()
        inFlight.decrementAndGet()
    }

    // --8<-- [start:doc-bingo-matchmaker-idle]
    fun closeIfIdle() {
        if (
            inFlight.get() == 0 && Duration.between(lastActivity, Instant.now()) >= IdleCloseAfter
        ) {
            instanceContext.close()
        }
    }

    // --8<-- [end:doc-bingo-matchmaker-idle]

    private companion object {
        val IdleCheckPeriod: Duration = Duration.ofSeconds(5)
        val IdleCloseAfter: Duration = Duration.ofSeconds(30)
    }
}
