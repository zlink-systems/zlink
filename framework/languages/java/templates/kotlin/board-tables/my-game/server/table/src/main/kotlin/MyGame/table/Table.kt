package MyGame.table

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

data class Seat(
    val playerId: String = "",
    var chips: Int = Rules.INITIAL_CHIPS,
    var cards: List<Int> = emptyList(),
    var acted: Boolean = false,
)

data class TableState(
    val seats: MutableList<Seat> = mutableListOf(),
    var deck: List<Int> = emptyList(),
    var handNumber: Int = 0,
    var inHand: Boolean = false,
    var turnSeat: Int = Rules.NO_SEAT,
    var pot: Int = 0,
)

class Table(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val clients: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = TableState()

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinTableRequest>()
        require(join.playerId == actorId && join.tableId == context.spotId())
        if (state.seats.none { it.playerId == actorId } && state.seats.size == Rules.SEATS)
            return ZLinkSpotActorJoinResult.reject(ActionRejected(Rules.TABLE_FULL))
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        val actorId = actor.context().actorId()
        if (state.seats.none { it.playerId == actorId }) state.seats.add(Seat(actorId))
        broadcastViews()
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    private fun tableView(playerId: String, seats: List<SeatView> = seatViews()) =
        TableView(
            context.spotId(),
            config.id,
            state.seats.indexOfFirst { it.playerId == playerId },
            state.handNumber,
            state.inHand,
            state.turnSeat,
            state.pot,
            seats,
        )

    suspend fun current(actor: Player) {
        val playerId = actor.context().actorId()
        reply(actor, tableView(playerId))
        val seat = state.seats.find { it.playerId == playerId }
        if (state.inHand && seat != null && seat.cards.isNotEmpty())
            reply(actor, HandDealt(state.handNumber, seat.cards))
    }

    suspend fun start(actor: Player) {
        val playerId = actor.context().actorId()
        val reason =
            when {
                state.seats.none { it.playerId == playerId } -> Rules.NOT_SEATED
                state.inHand -> Rules.HAND_IN_PROGRESS
                state.seats.size < Rules.MIN_PLAYERS -> Rules.NOT_ENOUGH_PLAYERS
                else -> null
            }
        if (reason != null) {
            reject(actor, reason)
            return
        }
        val deck = (0 until Rules.DECK_SIZE).toMutableList().apply { shuffle() }
        state.seats.forEach {
            it.cards = List(Rules.CARDS) { deck.removeAt(deck.lastIndex) }
            it.acted = false
        }
        state.deck = deck
        state.handNumber++
        state.inHand = true
        state.turnSeat = 0
        state.seats.forEach { seat -> send(seat.playerId, HandDealt(state.handNumber, seat.cards)) }
        broadcastViews()
    }

    suspend fun bet(actor: Player, request: BetRequest) {
        val playerId = actor.context().actorId()
        val index = state.seats.indexOfFirst { it.playerId == playerId }
        val reason =
            when {
                index == Rules.NO_SEAT -> Rules.NOT_SEATED
                !state.inHand || request.handNumber != state.handNumber -> Rules.NO_HAND
                index != state.turnSeat -> Rules.NOT_YOUR_TURN
                request.amount < 0 || request.amount > state.seats[index].chips ->
                    Rules.INVALID_AMOUNT
                else -> null
            }
        if (reason != null) {
            reject(actor, reason)
            return
        }
        val seat = state.seats[index]
        seat.chips -= request.amount
        state.pot += request.amount
        seat.acted = true
        state.turnSeat = state.seats.indexOfFirst { it.cards.isNotEmpty() && !it.acted }
        broadcastViews()
        if (state.turnSeat == Rules.NO_SEAT) settle()
    }

    private suspend fun settle() {
        val hands =
            state.seats.mapIndexedNotNull { i, seat ->
                if (seat.cards.isEmpty()) null else RevealedHand(i, seat.playerId, seat.cards)
            }
        val scores =
            hands.associate {
                it.seat to it.cards.sumOf { card -> card % Rules.RANKS + Rules.MIN_RANK }
            }
        val winningScore = scores.values.max()
        val winners = scores.filterValues { it == winningScore }.keys.sorted()
        val pot = state.pot
        winners.forEach { state.seats[it].chips += pot / winners.size }
        state.seats[winners.first()].chips += pot % winners.size
        state.inHand = false
        state.pot = 0
        state.seats.forEach { it.cards = emptyList() }
        val result = HandResult(state.handNumber, pot, winners, hands, seatViews())
        state.seats.forEach { send(it.playerId, result) }
        broadcastViews()
    }

    private fun seatViews() =
        state.seats.mapIndexed { i, seat ->
            SeatView(i, seat.playerId, seat.chips, seat.acted, seat.cards.size)
        }

    private suspend fun broadcastViews() {
        val seats = seatViews()
        state.seats.forEach { seat -> send(seat.playerId, tableView(seat.playerId, seats)) }
    }

    private suspend fun reject(actor: Player, reason: String) = reply(actor, ActionRejected(reason))

    private suspend fun reply(actor: Player, message: Any) {
        actor.context().boundSession().kotlin().send(message).await()
    }

    private suspend fun send(playerId: String, message: Any) {
        clients.kotlin().sendToActor(playerId, message).await()
    }
}

class TableTransfer : ZLinkSpotRelocationAdapter<Table> {
    override fun capture(
        spot: Table,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(Json.mapper.writeValueAsBytes(spot.state))
    }

    override fun restore(
        spot: Table,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.state = Json.mapper.readValue(state, TableState::class.java)
        return CompletableFuture.completedFuture(null)
    }
}

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun join(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinTableRequest,
    ) {
        require(
            request.playerId == actor.context().actorId() &&
                request.tableId == table.context.spotId()
        )
        table.current(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: TableViewRequest,
    ) {
        table.current(actor)
    }

    @ZLinkSpotActorSend
    suspend fun start(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: StartHandRequest,
    ) {
        table.start(actor)
    }

    @ZLinkSpotActorSend
    suspend fun bet(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: BetRequest,
    ) {
        table.bet(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun tableView(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: TableView,
    ) {
        actor.context().boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun dealt(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: HandDealt,
    ) {
        actor.context().boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun result(
        table: Table,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: HandResult,
    ) {
        actor.context().boundSession().kotlin().send(message).await()
    }
}
