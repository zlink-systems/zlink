package MyGame.table

import MyGame.*
import java.lang.reflect.Proxy
import java.security.SecureRandom
import java.util.Collections
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
    var chips: Long = Rules.INITIAL_CHIPS,
    var cards: List<Int> = emptyList(),
    var acted: Boolean = false,
)

data class TableState(
    val seats: MutableList<Seat> = mutableListOf(),
    var deck: List<Int> = emptyList(),
    var handNumber: Long = 0L,
    var inHand: Boolean = false,
    var turnSeat: Int = Rules.NO_SEAT,
    var pot: Long = 0L,
)

class Table(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val clients: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    private var randomIndex: (Int) -> Int = SecureRandom()::nextInt

    private constructor(
        context: ZLinkSpotContext,
        config: NodeConfig,
        clients: ZLinkActorClient,
        randomIndex: (Int) -> Int,
    ) : this(context, config, clients) {
        this.randomIndex = randomIndex
    }

    var state = TableState()

    companion object {
        internal fun forDomainChecks(randomIndex: (Int) -> Int): Table =
            Table(
                inertProxy(ZLinkSpotContext::class.java),
                NodeConfig(),
                inertProxy(ZLinkActorClient::class.java),
                randomIndex,
            )

        @Suppress("UNCHECKED_CAST")
        private fun <T : Any> inertProxy(type: Class<T>): T =
            Proxy.newProxyInstance(type.classLoader, arrayOf(type)) { _, method, _ ->
                when (method.returnType) {
                    java.lang.Boolean.TYPE -> false
                    java.lang.Integer.TYPE -> 0
                    java.lang.Long.TYPE -> 0L
                    else -> null
                }
            } as T
    }

    internal data class SettlementPlan(
        val pot: Long,
        val winners: List<Int>,
        val hands: List<RevealedHand>,
        val winnerBalances: List<Long>,
    )

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinTableRequest>()
        require(join.playerId == actorId && join.tableId == context.spotId())
        return if (admit(actorId)) ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) = broadcastViews()

    // Seats the player at admission; a seated player is admitted again without a new seat.
    internal fun admit(actorId: String): Boolean {
        if (state.seats.any { it.playerId == actorId }) return true
        if (state.seats.size == Rules.SEATS) return false
        state.seats.add(Seat(actorId))
        return true
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
        if (state.seats.any { it.playerId == playerId })
            reply(actor, HandDealt(state.handNumber, privateHand(playerId)))
    }

    suspend fun start(actor: Player) {
        val playerId = actor.context().actorId()
        val reason = startDomain(playerId)
        if (reason != null) {
            reject(actor, reason)
            return
        }
        state.seats.forEach { seat ->
            send(seat.playerId, HandDealt(state.handNumber, privateHand(seat.playerId)))
        }
        broadcastViews()
    }

    internal fun startDomain(playerId: String): String? {
        val reason =
            when {
                state.seats.none { it.playerId == playerId } -> Rules.NOT_SEATED
                state.inHand -> Rules.HAND_IN_PROGRESS
                state.seats.size < Rules.MIN_PLAYERS -> Rules.NOT_ENOUGH_PLAYERS
                else -> null
            }
        if (reason != null) return reason
        val nextHandNumber = Rules.tryAddContractIntegers(state.handNumber, 1L)
        if (nextHandNumber == null) return Rules.INTEGER_OUT_OF_RANGE
        val deck = (0 until Rules.DECK_SIZE).toMutableList()
        for (i in deck.lastIndex downTo 1) {
            val selected = randomIndex(i + 1)
            require(selected in 0..i) { "Random index is outside its bound" }
            Collections.swap(deck, i, selected)
        }
        state.seats.forEach {
            it.cards = List(Rules.CARDS) { deck.removeAt(deck.lastIndex) }
            it.acted = false
        }
        state.deck = deck
        state.handNumber = nextHandNumber
        state.inHand = true
        state.turnSeat = 0
        state.pot = 0L
        return null
    }

    suspend fun bet(actor: Player, request: BetRequest) {
        val playerId = actor.context().actorId()
        val outcome = betDomain(playerId, request)
        if (outcome.rejection != null) {
            reject(actor, outcome.rejection)
            return
        }
        broadcastViews()
        outcome.settlement?.let { settle(it) }
    }

    internal fun betDomain(playerId: String, request: BetRequest): BetOutcome {
        val index = state.seats.indexOfFirst { it.playerId == playerId }
        val reason =
            when {
                index == Rules.NO_SEAT -> Rules.NOT_SEATED
                request.handNumber > Rules.MAX_CONTRACT_INTEGER ||
                    request.amount > Rules.MAX_CONTRACT_INTEGER -> Rules.INTEGER_OUT_OF_RANGE
                !state.inHand || request.handNumber != state.handNumber -> Rules.NO_HAND
                index != state.turnSeat -> Rules.NOT_YOUR_TURN
                request.amount < 0 || request.amount > state.seats[index].chips ->
                    Rules.INVALID_AMOUNT
                else -> null
            }
        if (reason != null) return BetOutcome(reason, null)
        val seat = state.seats[index]
        val nextPot = Rules.tryAddContractIntegers(state.pot, request.amount)
        if (nextPot == null) return BetOutcome(Rules.INTEGER_OUT_OF_RANGE, null)
        val nextSeat = nextSeat(index)
        val settlement =
            if (nextSeat == Rules.NO_SEAT) createSettlement(index, request.amount, nextPot)
            else null
        if (nextSeat == Rules.NO_SEAT && settlement == null) {
            return BetOutcome(Rules.INTEGER_OUT_OF_RANGE, null)
        }

        seat.chips -= request.amount
        state.pot = nextPot
        seat.acted = true
        state.turnSeat = nextSeat
        return BetOutcome(null, settlement)
    }

    private fun nextSeat(currentSeat: Int): Int =
        state.seats.indices.firstOrNull {
            it != currentSeat && state.seats[it].cards.isNotEmpty() && !state.seats[it].acted
        } ?: Rules.NO_SEAT

    private fun createSettlement(
        bettor: Int,
        betAmount: Long,
        pot: Long,
    ): SettlementPlan? {
        val hands =
            state.seats.mapIndexedNotNull { i, seat ->
                if (seat.cards.isEmpty()) null else RevealedHand(i, seat.playerId, seat.cards)
            }
        val scores =
            hands.associate {
                it.seat to it.cards.sumOf { card -> card % Rules.RANKS + Rules.MIN_RANK }
            }
        val winningScore = scores.values.maxOrNull() ?: return null
        val winners = scores.filterValues { it == winningScore }.keys.sorted()
        if (winners.isEmpty()) return null

        val share = pot / winners.size
        val remainder = pot % winners.size
        val winnerBalances = mutableListOf<Long>()
        for (index in winners.indices) {
            val winner = winners[index]
            var balance =
                state.seats[winner].chips - if (winner == bettor) betAmount else 0L
            balance = Rules.tryAddContractIntegers(balance, share) ?: return null
            if (index == 0 && remainder != 0L)
                balance = Rules.tryAddContractIntegers(balance, remainder) ?: return null
            winnerBalances.add(balance)
        }
        return SettlementPlan(pot, winners, hands, winnerBalances)
    }

    private suspend fun settle(plan: SettlementPlan) {
        val result = applySettlement(plan)
        state.seats.forEach { send(it.playerId, result) }
        broadcastViews()
    }

    internal fun settleDomain(outcome: BetOutcome): HandResult? =
        outcome.settlement?.let(::applySettlement)

    private fun applySettlement(plan: SettlementPlan): HandResult {
        state.inHand = false
        state.turnSeat = Rules.NO_SEAT
        state.pot = 0L
        state.seats.forEach { it.cards = emptyList() }
        plan.winners.forEachIndexed { index, winner ->
            state.seats[winner].chips = plan.winnerBalances[index]
        }
        return HandResult(state.handNumber, plan.pot, plan.winners, plan.hands, seatViews())
    }

    internal fun privateHand(playerId: String): List<Int> =
        state.seats.firstOrNull { it.playerId == playerId }?.cards ?: emptyList()

    internal data class BetOutcome(val rejection: String?, val settlement: SettlementPlan?)

    private fun seatViews() =
        state.seats.mapIndexed { i, seat ->
            SeatView(i, seat.playerId, seat.chips, seat.acted, seat.cards.size)
        }

    private suspend fun broadcastViews() {
        val seats = seatViews()
        state.seats.forEach { seat -> send(seat.playerId, tableView(seat.playerId, seats)) }
    }

    private suspend fun reject(actor: Player, reason: String) =
        reply(actor, ActionRejected(reason, 0))

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
