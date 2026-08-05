package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot

import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomSettingsPayload
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.EnsurePlayerActorReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsRes

class BingoEntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingEntrySpot<PlayerActor>() {
    override suspend fun onCreateActorSuspending(
        actor: PlayerActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        val request = createRequest.decode(EnsurePlayerActorReq::class.java)
        actor.setDisplayName(request.displayName)
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: PlayerActor) {
        if (actor.destroyAfterEntrySpotJoin) {
            context.destroyActor(actor).await()
        }
    }

    override suspend fun onLeaveActorSuspending(actor: PlayerActor) = Unit

    override suspend fun onDisconnectActorSuspending(actor: PlayerActor) {
        actor.markDisconnected()
    }

    suspend fun observeEvents(
        actor: PlayerActor,
        request: ObserveBingoEventsReq,
    ): ObserveBingoEventsRes {
        val observerSpotId = "observe:${request.roomId}:${actor.actorId()}"
        val settings = BingoRoomSettings.createObserver(
            request.roomId,
            actor.actorId(),
            SampleTimings.DrawPeriod.toMillis(),
        )
        val settingsPayload = BingoRoomSettingsPayload(
            mode = settings.mode,
            roomName = settings.roomName,
            requiredPlayers = settings.requiredPlayers,
            maxDrawNumber = settings.maxDrawNumber,
            purpose = settings.purpose,
            observedRoomId = settings.observedRoomId ?: "",
        )
        spots.getOrCreate(observerSpotId, SampleNames.RoomSpotType)
            .inMesh(SampleNames.Mesh)
            .request(settingsPayload)
            .submit()
            .await()
        actor.context().joinSpot(
            observerSpotId,
            BingoRoomJoinReq(
                request.roomId,
                actor.actorId(),
                actor.displayName,
                true,
            ),
        ).defer()
        return ObserveBingoEventsRes(true)
    }
}
