package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot

import org.slf4j.LoggerFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomCreateReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomSettingsPayload
import systems.zlink.samples.kotlin.bingo.shared.contracts.EnsurePlayerActorReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsRes

class BingoEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingEntrySpot<PlayerActor>() {
    private val kotlinSpots = spots.kotlin()
    private val logger = LoggerFactory.getLogger(BingoEntrySpot::class.java)

    override suspend fun onCreateActorSuspending(
        actor: PlayerActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        val request = createRequest.decode<EnsurePlayerActorReq>()
        actor.setDisplayName(request.displayName)
        return ZLinkActorCreateResponse.accept()
    }

    // --8<-- [start:doc-bingo-entry-destroy]
    override suspend fun onJoinedActorSuspending(actor: PlayerActor) {
        if (actor.destroyAfterEntrySpotJoin) {
            context.destroyActor(actor).await()
            logger.info("bingo-lifecycle entry-destroy-complete actor={}", actor.actorId())
        }
    }

    // --8<-- [end:doc-bingo-entry-destroy]

    override suspend fun onLeaveActorSuspending(actor: PlayerActor) {}

    override suspend fun onDisconnectActorSuspending(actor: PlayerActor) {
        actor.markDisconnected()
    }

    suspend fun observeEvents(
        actor: PlayerActor,
        request: ObserveBingoEventsReq,
    ): ObserveBingoEventsRes {
        val observerSpotId = "observe:${request.roomId}:${actor.actorId()}"
        val settings =
            BingoRoomSettings.createObserver(
                request.roomId,
                actor.actorId(),
                SampleTimings.DrawPeriod.toMillis(),
            )
        val settingsPayload =
            BingoRoomSettingsPayload(
                mode = settings.mode,
                roomName = settings.roomName,
                requiredPlayers = settings.requiredPlayers,
                maxDrawNumber = settings.maxDrawNumber,
                purpose = settings.purpose,
                observedRoomId = settings.observedRoomId ?: "",
            )
        kotlinSpots
            .getOrCreate(observerSpotId, SampleNames.RoomSpotType)
            .inMesh(SampleNames.Mesh)
            .request(BingoRoomCreateReq(settingsPayload))
            .await()
        actor
            .context()
            .joinSpot(
                observerSpotId,
                BingoRoomJoinReq(request.roomId, actor.actorId(), actor.displayName, true),
            )
            .defer()
        return ObserveBingoEventsRes(true)
    }
}
