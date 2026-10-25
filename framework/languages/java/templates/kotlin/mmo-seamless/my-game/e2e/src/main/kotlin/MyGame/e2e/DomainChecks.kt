package MyGame.e2e

import MyGame.*
import MyGame.zone.ZonePositionDomain

private object DomainChecks {
    private var passed = 0
    private var assertions = 0

    fun run() {
        passed = 0
        assertions = 0
        check("non-finite movement", ::rejectsNonFiniteMovement)
        check("step limit", ::rejectsOversizedStep)
        check("world boundary", ::rejectsOutsideWorld)
        check("movement budget", ::rejectsUntilServerRetry)
        check("stale and unsafe sequence", ::rejectsInvalidSequence)
        check("initial location resolution", ::resolvesInitialLocation)
        check("remaining-time relocation snapshot", ::restoresRemainingBudget)
        check("presence lifecycle", ::tracksPresenceLifecycle)
        check("enum and nullable JSON", ::preservesJsonContract)
        println("Domain checks: $passed/9 passed; assertions: $assertions")
    }

    private fun check(name: String, action: () -> Unit) {
        action()
        println("PASS $name")
        passed++
    }

    private fun require(condition: Boolean, message: String) {
        assertions++
        check(condition) { message }
    }

    private fun create(): Pair<ZonePositionDomain, ManualClock> {
        val clock = ManualClock()
        val domain = ZonePositionDomain("zone-2", clock::readMilliseconds)
        domain.addPlayer("player", EnterZone(Position(100.0), 0, 0))
        return domain to clock
    }

    private fun rejectsNonFiniteMovement() {
        val (domain) = create()
        listOf(Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY).forEach { delta ->
            requireRejected(domain, delta, 1, "non-finite")
        }
    }

    private fun rejectsOversizedStep() {
        val (domain) = create()
        requireRejected(domain, Rules.MAX_STEP + 1, 1, "step-too-large")
    }

    private fun rejectsOutsideWorld() {
        val domain = ZonePositionDomain("zone-6") { 0L }
        domain.addPlayer("edge", EnterZone(Position((Rules.WORLD_WIDTH - 1).toDouble()), 0, 0))
        requireRejected(domain, 1.0, 1, "outside-world", "edge")
    }

    private fun rejectsUntilServerRetry() {
        val (domain, clock) = create()
        val first = domain.move("player", MoveRequest(Rules.MAX_STEP, 1))
        require(first.accepted && first.position.x == 110.0, "Initial move must use MaxStep budget")
        val before = domain.state("player")
        val rejected = domain.move("player", MoveRequest(Rules.MAX_STEP, 2))
        val rejection = rejected.rejection ?: error("Expected move-budget rejection")
        require(rejection.reason == "move-budget", "Expected move-budget rejection")
        require(rejection.retryAfterMs > 0, "move-budget must report a positive retry delay")
        require(domain.state("player") == before, "Rejected move changed position or sequence")
        clock.advance(rejection.retryAfterMs)
        val accepted = domain.move("player", MoveRequest(Rules.MAX_STEP, 2))
        require(accepted.accepted && accepted.position.x == 120.0, "Server retry delay was insufficient")
    }

    private fun rejectsInvalidSequence() {
        val (domain) = create()
        requireRejected(domain, 1.0, 0, "stale-sequence")
        requireRejected(domain, 1.0, Rules.MAX_SAFE_INTEGER + 1, "integer-out-of-range")
        require(Rules.MAX_SAFE_INTEGER == 9_007_199_254_740_991L, "Contract integer limit changed")

        val (gapDomain) = create()
        val gap = gapDomain.move("player", MoveRequest(1.0, 2))
        require(gap.accepted && gap.sequence == 2L, "A sequence gap was rejected")

        val maxDomain = ZonePositionDomain("zone-2") { 0L }
        maxDomain.addPlayer("last", EnterZone(Position(100.0), Rules.MAX_SAFE_INTEGER, 0))
        requireRejected(maxDomain, 1.0, Rules.MAX_SAFE_INTEGER, "stale-sequence", "last")
        requireRejected(maxDomain, 1.0, Long.MAX_VALUE, "integer-out-of-range", "last")

        val safeArrival = ZonePositionDomain("zone-2") { 0L }
        require(
            safeArrival.queueArrival("safe", EnterZone(Position(100.0), Rules.MAX_SAFE_INTEGER, 0)) == null,
            "Maximum safe sequence was rejected during admission",
        )
        val unsafeArrival = ZonePositionDomain("zone-2") { 0L }
        val arrivalRejection =
            unsafeArrival.queueArrival("unsafe", EnterZone(Position(100.0), Rules.MAX_SAFE_INTEGER + 1, 0))
        require(
            arrivalRejection?.reason == "integer-out-of-range" && unsafeArrival.takeArrival("unsafe") == null,
            "Out-of-range admission was accepted or stored",
        )
    }

    private fun resolvesInitialLocation() {
        val valid = ZonePositionDomain.resolveLocation(299.0)
        require(valid.zoneId == "zone-3" && valid.rejection == null, "Initial position resolved incorrectly")
        require(ZonePositionDomain.resolveLocation(Double.NaN).rejection?.reason == "non-finite", "NaN not rejected")
        require(
            ZonePositionDomain.resolveLocation(Double.POSITIVE_INFINITY).rejection?.reason == "non-finite",
            "Infinity not rejected",
        )
        require(
            ZonePositionDomain.resolveLocation(Rules.WORLD_WIDTH.toDouble()).rejection?.reason == "outside-world",
            "World limit not rejected",
        )
    }

    private fun restoresRemainingBudget() {
        val (source, sourceClock) = create()
        require(source.move("player", MoveRequest(Rules.MAX_STEP, 1)).accepted, "Expected initial move")
        sourceClock.advance(75)
        val snapshot = source.capture()
        val destinationClock = ManualClock(500)
        val destination = ZonePositionDomain("zone-2", destinationClock::readMilliseconds)
        destination.restore(snapshot)
        val rejection = destination.move("player", MoveRequest(5.0, 2)).rejection
            ?: error("Expected relocation budget rejection")
        require(rejection.reason == "move-budget", "Relocation lost movement budget")
        require(rejection.retryAfterMs == 25, "Relocation did not preserve remaining milliseconds")
    }

    private fun tracksPresenceLifecycle() {
        val (domain) = create()
        domain.addPlayer("observer", EnterZone(Position(101.0), 0, 0))
        require(domain.connectedPlayers().size == 2, "Connected players were not tracked")
        val before = domain.state("player")
        require(domain.setConnected("player", false)?.connected == false, "Disconnect did not end visibility")
        require(domain.connectedPlayers().size == 1, "Disconnected player remained visible")
        require(domain.state("player").position == before.position, "Disconnect erased position")
        require(domain.setConnected("player", true)?.connected == true, "Reconnect did not restore visibility")
        require(domain.state("player").position == before.position, "Reconnect changed position")
        require(domain.removePlayer("player")?.position == before.position, "Zone leave lost last position")
        require(domain.connectedPlayers().size == 1, "Zone leave retained player")
    }

    private fun preservesJsonContract() {
        val left = Json.mapper.writeValueAsString(PlayerLeft("player", PlayerLeftReason.LEFT_ZONE))
        require(left.contains("\"reason\":\"left-zone\""), "PlayerLeft reason JSON changed")
        val border =
            Json.mapper.writeValueAsString(
                BorderPresence("zone-2", "player", Position(10.0), 1, BorderPresenceKind.MOVED, null)
            )
        require(border.contains("\"kind\":\"moved\""), "Border kind JSON changed")
        require(border.contains("\"reason\":null"), "Nullable border reason was not serialized as null")
        val rejected = Json.mapper.writeValueAsString(ActionRejected("outside-world", 0))
        require(rejected.contains("\"reason\":\"outside-world\""), "ActionRejected reason field changed")
        require(rejected.contains("\"retryAfterMs\":0"), "ActionRejected retry field changed")
        require(
            ActionRejected::class.java.getDeclaredField("retryAfterMs").type == Int::class.javaPrimitiveType,
            "RetryAfterMs must use a 32-bit integer",
        )
    }

    private fun requireRejected(
        domain: ZonePositionDomain,
        delta: Double,
        sequence: Long,
        reason: String,
        playerId: String = "player",
    ) {
        val before = domain.state(playerId)
        val rejection = domain.move(playerId, MoveRequest(delta, sequence)).rejection
            ?: error("Expected $reason rejection")
        require(rejection.reason == reason, "Expected $reason, received ${rejection.reason}")
        require(rejection.retryAfterMs == 0, "$reason must not report a retry delay")
        require(domain.state(playerId) == before, "Rejected $reason changed domain state")
    }

    private class ManualClock(initialMs: Long = 0) {
        private var now = initialMs
        fun readMilliseconds(): Long = now
        fun advance(milliseconds: Int) {
            now += milliseconds
        }
    }
}

fun runDomainChecks() = DomainChecks.run()
