package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot

import java.time.Duration
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.yieldReply
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomTimerHandler
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoGame
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomEvent
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomEventKind
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomGame
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoGameEndedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoGameStartedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoNumberDrawnNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomState
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRewardAnnouncedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoStateNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRewardAcquiredEvent
import systems.zlink.samples.kotlin.bingo.shared.contracts.PlayerJoinedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.GetPlayerRecordReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.GetPlayerRecordRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReportBingoResultReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReportBingoResultRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.SubmitBingoCardReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.SubmitBingoCardRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.card
import systems.zlink.samples.kotlin.bingo.shared.contracts.winners

class BingoRoomSpot(
    override val context: ZLinkSpotContext,
    private val settingsInitializer: BingoRoomSettingsInitializer,
) : ZLinkSuspendingSpot<PlayerActor>() {
    private val actors = mutableMapOf<String, PlayerActor>()
    private val observers = mutableMapOf<String, PlayerActor>()
    private val pendingJoins = mutableMapOf<String, BingoRoomJoinReq>()
    private val pendingObserverRewards = mutableListOf<BingoRewardAcquiredEvent>()
    private var settings = BingoRoomSettings.create(
        "two-player",
        0,
        SampleTimings.DrawPeriod.toMillis(),
    )
    private var game: BingoRoomGame? = BingoGame.room(context.spotId(), settings)
    private var timer: ZLinkTimer? = null
    private var cleanupStarted = false

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        settingsInitializer.handle(this, request)
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val joinRequest = request.decode(BingoRoomJoinReq::class.java)
        validateJoin(actorId, joinRequest)
        val preview = if (joinRequest.observeOnly) {
            observerJoinState(joinRequest)
        } else {
            requireGame().previewJoin(actorId, joinRequest.displayName)
        }
        pendingJoins[actorId] = joinRequest
        return ZLinkSpotActorJoinResult.accept(BingoRoomJoinRes(preview))
    }

    override suspend fun onJoinedActorSuspending(actor: PlayerActor) {
        val request = pendingJoins[actor.actorId()]
            ?: error("joined actor does not have a pending admission")
        if (request.observeOnly) {
            pendingJoins.remove(actor.actorId())
            join(actor, request, 0, 0)
            val pendingRewards = pendingObserverRewards.toList()
            pendingObserverRewards.clear()
            pendingRewards.forEach { notifyObservers(it) }
            return
        }
        val record = context.outbound()
            .requestToChannel(SampleNames.ApiChannel, GetPlayerRecordReq(actor.actorId()))
            .timeout(SampleTimings.RequestTimeout)
            .yieldReply<GetPlayerRecordRes>()
        if (pendingJoins[actor.actorId()] === request) {
            pendingJoins.remove(actor.actorId())
            join(actor, request, record.wins, record.losses)
        }
    }

    override suspend fun onLeaveActorSuspending(actor: PlayerActor) {
        if (!actors.containsKey(actor.actorId()) || game == null) {
            observers.remove(actor.actorId())
            return
        }
        val state = requireGame().snapshot()
        context.outbound()
            .requestToChannel(
                SampleNames.ApiChannel,
                ReportBingoResultReq(
                    state.roomId,
                    actor.actorId(),
                    state.winners.contains(actor.actorId()),
                    state.drawSeq,
                ),
            )
            .timeout(SampleTimings.RequestTimeout)
            .yieldReply<ReportBingoResultRes>()
        actors.remove(actor.actorId())
    }

    override suspend fun onDisconnectActorSuspending(actor: PlayerActor) {
        actor.markDisconnected()
    }

    override suspend fun onInitializeSuspending() {
        if (settings.observerMode()) {
            return
        }
        timer = context.addTimer(
            "bingo-draw",
            Duration.ofMillis(settings.drawPeriodMillis),
            BingoRoomTimerHandler::class.java,
            null,
        ).await()
    }

    override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext) {
        timer?.cancel()?.await()
    }

    override suspend fun onRelocationReadyCompletedSuspending(
        completion: ZLinkSpotRelocationReadyCompletion,
    ) = Unit

    fun join(
        actor: PlayerActor,
        request: BingoRoomJoinReq,
        wins: Int,
        losses: Int,
    ): BingoRoomJoinRes {
        validateJoin(actor.actorId(), request)
        actor.setDisplayName(request.displayName)
        actor.joinRoom(request.roomId)
        if (request.observeOnly) {
            observers[actor.actorId()] = actor
            context.relocationReady().defer()
            return BingoRoomJoinRes(observerJoinState(request))
        }
        val change = requireGame().join(actor.actorId(), request.displayName, wins, losses)
        actors[actor.actorId()] = actor
        publishEvents(
            change.events,
            { actorId -> if (actorId == actor.actorId()) null else actors[actorId] },
        )
        return BingoRoomJoinRes(change.state)
    }

    private fun validateJoin(actorId: String, request: BingoRoomJoinReq) {
        if (request.actorId != actorId) {
            throw IllegalStateException("Join request actor id does not match bound actor.")
        }
        if (!request.observeOnly && request.roomId != context.spotId()) {
            throw IllegalStateException("Join request room id does not match bingo room.")
        }
        if (request.observeOnly) {
            if (!settings.observerMode() || request.roomId != settings.observedRoomId) {
                throw IllegalStateException("Observe-only actor can join only its observer BingoRoom.")
            }
            return
        }
        if (settings.observerMode()) {
            throw IllegalStateException("Player actor cannot join an observer BingoRoom.")
        }
    }

    suspend fun submitCard(
        actor: PlayerActor,
        request: SubmitBingoCardReq,
    ): SubmitBingoCardRes {
        if (request.roomId != context.spotId()) {
            throw IllegalStateException("Submit request room id does not match bingo room.")
        }
        val change = requireGame().submitCard(actor.actorId(), request.card)
        publishEvents(change.events, actors::get)
        return SubmitBingoCardRes(change.state)
    }

    suspend fun tick() {
        val game = this.game
        if (game == null || cleanupStarted) {
            return
        }
        val change = game.drawNext()
        publishEvents(change.events, actors::get)
        publishWinner(change)
        leaveFinishedActors(change)
        if (change.state.status == BingoRoomGame.Finished) {
            context.relocationReady().defer()
        }
    }

    suspend fun announceReward(event: BingoRewardAcquiredEvent) {
        if (!settings.observerMode() || event.roomId != settings.observedRoomId) {
            return
        }
        if (observers.isEmpty()) {
            pendingObserverRewards += event
            return
        }
        notifyObservers(event)
        context.relocationReady().defer()
    }

    private suspend fun notifyObservers(event: BingoRewardAcquiredEvent) {
        for (observer in observers.values.toList()) {
            observer.push(
                BingoRewardAnnouncedNotify(
                    event.roomId,
                    event.actorId,
                    event.drawSeq,
                    event.itemId,
                    event.itemName,
                    event.rarity,
                )
            ).await()
        }
    }

    suspend fun stopObserving(
        actor: PlayerActor,
        request: StopObservingBingoEventsReq,
    ): StopObservingBingoEventsRes {
        if (!settings.observerMode() ||
            request.roomId != settings.observedRoomId ||
            !observers.containsKey(actor.actorId())
        ) {
            return StopObservingBingoEventsRes(false)
        }
        observers.remove(actor.actorId())
        context.leaveActor(actor).exceptionally { null }
        return StopObservingBingoEventsRes(true)
    }

    private suspend fun leaveFinishedActors(change: BingoRoomGame.Change) {
        if (cleanupStarted || change.state.status != BingoRoomGame.Finished) {
            return
        }
        cleanupStarted = true
        for (actor in actors.values.toList()) {
            actor.markForDestroyAfterRoomLeave()
            context.leaveActor(actor).await()
        }
    }

    fun applySettings(settings: BingoRoomSettings) {
        check(settings.observerMode() || settings.requiredPlayers > 0) { "Bingo room requires at least one player." }
        check(settings.maxDrawNumber > 0) { "Bingo room requires at least one draw number." }
        check(settings.drawPeriodMillis > 0) {
            "Bingo room draw period must be positive."
        }
        this.settings = settings
        game = if (settings.observerMode()) null else BingoGame.room(context.spotId(), settings)
        cleanupStarted = false
    }

    internal fun captureRelocationState(): RelocationState =
        RelocationState(
            settings,
            game?.snapshot() ?: BingoRoomState(
                context.spotId(),
                BingoRoomGame.Running,
                "",
                false,
                0,
                null,
                emptyList(),
                emptyList(),
                emptyList(),
            ),
        )

    internal fun restoreRelocationState(state: RelocationState) {
        settings = state.settings
        game = if (settings.observerMode()) {
            null
        } else {
            BingoRoomGame.restore(context.spotId(), settings, state.state)
        }
        cleanupStarted = false
    }

    internal data class RelocationState(
        val settings: BingoRoomSettings,
        val state: BingoRoomState,
    )

    private suspend fun publishWinner(change: BingoRoomGame.Change) {
        val state = change.state
        if (state.status != BingoRoomGame.Finished || state.winners.isEmpty()) {
            return
        }
        val winner = state.winners.first()
        context.outbound()
            .publish(
                SampleNames.RoomRewardChannel,
                SampleNames.WinnerTopic,
                BingoRewardAcquiredEvent(
                    state.roomId,
                    winner,
                    state.drawSeq,
                    "rare-golden-dauber",
                    "Golden Dauber",
                    "Legendary",
                ),
            )
            .submit()
            .await()
    }

    private fun publishEvents(
        events: List<BingoRoomEvent>,
        actorResolver: (String) -> PlayerActor?,
    ) {
        for (event in events) {
            publishEvent(event, actorResolver(event.recipientActorId))
        }
    }

    private fun publishEvent(
        event: BingoRoomEvent,
        recipient: PlayerActor?,
    ) {
        if (recipient == null) {
            return
        }
        when (event.kind) {
            BingoRoomEventKind.PLAYER_JOINED ->
                recipient.push(
                    PlayerJoinedNotify(
                        event.state.roomId,
                        event.joinedActorId!!,
                        event.joinedDisplayName!!,
                        event.seat,
                        event.host,
                        event.state,
                    )
                )

            BingoRoomEventKind.GAME_STARTED ->
                recipient.push(BingoGameStartedNotify(event.state))

            BingoRoomEventKind.NUMBER_DRAWN ->
                recipient.push(
                    BingoNumberDrawnNotify(
                        event.state.roomId,
                        event.state.drawSeq,
                        event.drawnNumber,
                        event.state,
                    )
                )

            BingoRoomEventKind.STATE ->
                recipient.push(BingoStateNotify(event.state))

            BingoRoomEventKind.GAME_ENDED ->
                recipient.push(BingoGameEndedNotify(event.state))
        }
    }

    private fun observerJoinState(
        request: BingoRoomJoinReq,
    ): BingoRoomState =
        BingoRoomState(
                request.roomId,
                BingoRoomGame.Running,
                "",
                false,
                0,
                null,
                emptyList(),
                emptyList(),
                emptyList(),
            )

    private fun requireGame(): BingoRoomGame =
        game ?: throw IllegalStateException("Observer BingoRoom does not own game state.")
}
