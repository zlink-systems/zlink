package systems.zlink.samples.kotlin.bingo.server.matchmaking

import java.time.Duration
import java.time.Instant
import java.util.concurrent.atomic.AtomicInteger
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext

class BingoMatchmaker(
    private val instanceContext: ZLinkInstanceSpotContext,
) : ZLinkInstanceSpot {
    private val inFlight = AtomicInteger()
    @Volatile
    private var lastActivity: Instant = Instant.now()

    override fun context(): ZLinkInstanceSpotContext = instanceContext

    override fun onInitialize(): java.util.concurrent.CompletionStage<Void> =
        instanceContext.addTimer(
            "matchmaker-idle-close",
            IdleCheckPeriod,
            BingoMatchmakerIdleTimerHandler::class.java,
            null,
        ).thenApply { null }

    fun beginRequest() {
        inFlight.incrementAndGet()
        lastActivity = Instant.now()
    }

    fun endRequest() {
        lastActivity = Instant.now()
        inFlight.decrementAndGet()
    }

    fun closeIfIdle() {
        if (inFlight.get() == 0 && Duration.between(lastActivity, Instant.now()) >= IdleCloseAfter) {
            instanceContext.close()
        }
    }

    private companion object {
        val IdleCheckPeriod: Duration = Duration.ofSeconds(5)
        val IdleCloseAfter: Duration = Duration.ofSeconds(30)
    }

}
