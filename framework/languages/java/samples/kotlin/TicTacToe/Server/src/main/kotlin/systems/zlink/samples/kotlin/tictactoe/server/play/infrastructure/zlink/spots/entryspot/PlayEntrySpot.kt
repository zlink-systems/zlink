package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot

import kotlinx.coroutines.future.await
import org.slf4j.LoggerFactory
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayActorJoinGameHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayActorObserveMilestoneHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayerWinMilestoneMsgHandler
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.ObserveMilestoneRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerWinMilestoneMsg
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.WinMilestoneNotify

// --8<-- [start:doc-entry-spot]
class PlayEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val settings: SampleSettings,
) : ZLinkSuspendingEntrySpot<PlayActor>() {
    private val milestoneObservers = mutableListOf<PlayActor>()

    override suspend fun onCreateActorSuspending(
        actor: PlayActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        if (createRequest.isEmpty) {
            return ZLinkActorCreateResponse.accept()
        }
        actor.applyPlayer(createRequest.decode(PlayerInfo::class.java))
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: PlayActor) {
        if (actor.destroyAfterEntrySpotJoin) {
            context.destroyActor(actor).await()
            logger.info("tictactoe actor destroy completed actor={}", actor.actorId)
        }
    }

    override suspend fun onLeaveActorSuspending(actor: PlayActor) {
        milestoneObservers.removeIf { it.actorId == actor.actorId }
    }

    override suspend fun onDisconnectActorSuspending(actor: PlayActor) {
        actor.markDisconnected()
        milestoneObservers.removeIf { it.actorId == actor.actorId }
    }

    fun observeMilestone(actor: PlayActor): ObserveMilestoneRes {
        rememberObserver(actor)
        return ObserveMilestoneRes(true)
    }

    fun notifyMilestone(event: PlayerWinMilestoneMsg) {
        val payload = WinMilestoneNotify(
            roomId = event.roomId,
            actorId = event.actorId,
            displayName = event.displayName,
            wins = event.wins,
        )
        milestoneObservers.toList().forEach { observer ->
            observer.context().boundSession()
                .send(payload)
                .submit()
        }
    }

    private fun rememberObserver(actor: PlayActor) {
        milestoneObservers.removeIf { it.actorId == actor.actorId }
        milestoneObservers += actor
    }

    private companion object {
        val logger = LoggerFactory.getLogger(PlayEntrySpot::class.java)
    }
}
// --8<-- [end:doc-entry-spot]
