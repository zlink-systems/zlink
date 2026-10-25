package MyGame.e2e

import MyGame.*
import java.util.UUID

private fun sameDungeon(before: DungeonView, after: DungeonView) {
    check(before.copy(nodeId = after.nodeId) == after) {
        "Dungeon state changed: $before -> $after"
    }
}

private suspend fun enter(leader: Client, member: Client): DungeonView {
    val forming =
        leader.sendFor(TownView::class.java, CreatePartyRequest()) { view ->
            view.parties.any { it.members == listOf(leader.playerId) }
        }
    val partyId = forming.parties.single { it.members == listOf(leader.playerId) }.partyId
    check(Names.validId(partyId, "party-")) { "Party ID format: $partyId" }

    member.send(JoinPartyRequest(partyId))
    val ready: (TownView) -> Boolean = { view ->
        view.parties.any { it.partyId == partyId && it.members == listOf(leader.playerId, member.playerId) }
    }
    check(leader.next(ready) == member.next<TownView>(ready)) { "Party views differ" }

    val rejected = member.sendFor(ActionRejected::class.java, EnterDungeonRequest()) { true }
    check(rejected == ActionRejected(Rejection.NOT_LEADER)) { "Nonleader was not rejected: $rejected" }
    val unchanged = member.sendFor(TownView::class.java, ViewRequest()) { true }
    val unchangedParty = unchanged.parties.single { it.partyId == partyId }
    check(unchangedParty.phase == RunPhase.Forming && unchangedParty.runId == null) {
        "Rejected entry changed party: $unchangedParty"
    }

    val internalRejection = member.sendFor(
        ActionRejected::class.java,
        JoinDungeon("forged", "forged"),
    ) { true }
    check(internalRejection == ActionRejected(Rejection.PACKET_NOT_ALLOWED)) {
        "Session relayed internal packet: $internalRejection"
    }
    val stillUnchanged = member.sendFor(TownView::class.java, ViewRequest()) { true }
    check(Json.mapper.writeValueAsString(unchanged) == Json.mapper.writeValueAsString(stillUnchanged)) {
        "Internal rejection changed party: $unchanged -> $stillUnchanged"
    }

    leader.send(EnterDungeonRequest())
    leader.send(EnterDungeonRequest())
    val active: (DungeonView) -> Boolean = { it.partyId == partyId && it.phase == RunPhase.Active }
    val leaderView = leader.next(active)
    val memberView = member.next(active)
    check(leaderView == memberView) { "Dungeon views differ: $leaderView -> $memberView" }
    check(
        leaderView.dungeonId == Names.dungeonId(leaderView.runId) &&
            Names.validId(leaderView.runId, "run-") &&
            leaderView.stage == 0L &&
            leaderView.members == listOf(leader.playerId, member.playerId),
    ) { "Unexpected Dungeon view: $leaderView" }
    sameDungeon(leaderView, leader.view { it == leaderView })
    sameDungeon(leaderView, member.view { it == leaderView })
    return leaderView
}

private suspend fun secondParty(endpoint: String, replacement: String? = null, first: DungeonView? = null) {
    val suffix = UUID.randomUUID().toString()
    val leader = Client(endpoint, "p3-$suffix")
    val member = Client(endpoint, "p4-$suffix")
    withCleanup(cleanup = { withCleanup(cleanup = { leader.close() }) { member.close() } }) {
        leader.connect()
        member.connect()
        leader.join(1)
        leader.next<TownView> { it.townId == Names.townId(1) }
        member.join(1)
        member.next<TownView> { it.players.containsAll(listOf(leader.playerId, member.playerId)) }
        val fresh = enter(leader, member)
        check(first == null || (fresh.dungeonId != first.dungeonId && fresh.runId != first.runId && fresh.partyId != first.partyId)) {
            "Two town parties shared an identity"
        }
        check(replacement == null || fresh.nodeId == replacement) { "Replacement did not accept new work" }
    }
}

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val leader = Client(nodes.endpoint, "p1-$suffix")
    val member = if (id == "channel") null else Client(nodes.endpoint, "p2-$suffix")
    var memberClosed = false
    withCleanup(
        cleanup = {
            withCleanup(cleanup = { leader.close() }) {
                if (!memberClosed) member?.close()
            }
        }
    ) {
        leader.connect()
        if (id == "channel") {
            leader.join(1)
            val town = leader.next<TownView> { it.townId == Names.townId(1) }
            check(town.nodeId == "townA") { "Initial town was not hosted on townA" }
            val before = leader.sessionInfo()

            awaitUsingClient(nodes.endpoint, "channel-observer-$suffix") { observer ->
                observer.connect()
                observer.join(1)
                observer.next<TownView> { it.players.containsAll(listOf(leader.playerId, observer.playerId)) }
                val forming =
                    leader.sendFor(TownView::class.java, CreatePartyRequest()) { view ->
                        view.parties.any { it.members == listOf(leader.playerId) }
                    }
                val partyId = forming.parties.single { it.members == listOf(leader.playerId) }.partyId
                val badChannel =
                    leader.sendFor(ActionRejected::class.java, ChangeChannelRequest(Names.MAXIMUM_INTEGER + 1)) { true }
                check(badChannel == ActionRejected(Rejection.INTEGER_OUT_OF_RANGE)) { "Channel range rejection: $badChannel" }
                val unchanged = leader.sendFor(TownView::class.java, ViewRequest()) { true }
                check(Json.mapper.writeValueAsString(forming) == Json.mapper.writeValueAsString(unchanged)) {
                    "Rejected channel changed town"
                }

                leader.send(ChangeChannelRequest(2))
                val changed = leader.next<TownView> { it.townId == Names.townId(2) }
                val sourceLeft = observer.next<TownView> {
                    leader.playerId !in it.players && it.parties.none { party -> party.partyId == partyId }
                }
                check(changed.nodeId == "townB" && leader.playerId in changed.players) {
                    "Channel relocation failed: $changed"
                }
                check(changed.parties.none { it.partyId == partyId }) { "Forming party followed channel change" }
                check(sourceLeft.players.none { it == leader.playerId }) { "Source town retained player" }
            }
            check(leader.sessionInfo() == before) { "Channel change altered Session binding" }
        } else {
            requireNotNull(member)
            member.connect()
            leader.join()
            leader.next<TownView> { it.townId == Names.townId(2) }
            member.join()
            member.next<TownView> { it.players.containsAll(listOf(leader.playerId, member.playerId)) }
            val leaderSession = leader.sessionInfo()
            val memberSession = member.sessionInfo()
            val dungeon = enter(leader, member)

            val channelRejected = leader.sendFor(ActionRejected::class.java, ChangeChannelRequest(1)) { true }
            check(channelRejected == ActionRejected(Rejection.RUN_IN_PROGRESS)) {
                "Running member changed channel: $channelRejected"
            }
            sameDungeon(dungeon, leader.view { it == dungeon })

            leader.send(AdvanceRequest())
            var advanced = leader.next<DungeonView> { it.stage == 1L }
            check(advanced == member.next<DungeonView> { it.stage == 1L } && advanced.nodeId == dungeon.nodeId)
            check(
                leader.sessionInfo() == leaderSession && member.sessionInfo() == memberSession,
            ) { "Session identity changed while entering Dungeon" }

            when (id) {
                "dungeon" -> {
                    var stage = 2L
                    while (stage <= Names.FINAL_STAGE) {
                        leader.send(AdvanceRequest())
                        advanced = leader.next { it.stage == stage }
                        sameDungeon(advanced, member.next { it.stage == stage })
                        stage++
                    }
                    check(advanced.phase == RunPhase.Completed) { "Final stage did not complete run" }
                    val completedParty = leader.sendFor(TownView::class.java, ViewRequest()) { view ->
                        view.parties.any {
                            it.partyId == advanced.partyId &&
                                it.phase == RunPhase.Completed &&
                                it.runId == advanced.runId
                        }
                    }
                    check(completedParty.parties.single { it.partyId == advanced.partyId }.phase == RunPhase.Completed) {
                        "Party owner did not complete the run"
                    }
                    val completedRejection = leader.sendFor(ActionRejected::class.java, AdvanceRequest()) { true }
                    check(completedRejection == ActionRejected(Rejection.RUN_NOT_ACTIVE)) {
                        "Completed advance accepted: $completedRejection"
                    }
                    sameDungeon(advanced, leader.view { it == advanced })
                    secondParty(nodes.endpoint, first = advanced)
                }
                "reconnect" -> {
                    memberClosed = true
                    member.close()
                    awaitUsingClient(nodes.endpoint, member.playerId) { reconnected ->
                        reconnected.join()
                        val restored = reconnected.next<DungeonView>()
                        check(restored == advanced) { "Reconnect changed Dungeon state: $restored -> $advanced" }
                        val identity = reconnected.sessionInfo()
                        check(
                            identity.sessionId != memberSession.sessionId &&
                                identity.actorId == memberSession.actorId &&
                                identity.objectGeneration == memberSession.objectGeneration,
                        ) { "Reconnect changed Actor identity" }
                    }
                }
                "update" -> {
                    nodes.drain(advanced.nodeId)
                    val relocated = leader.view { it.nodeId != advanced.nodeId }
                    sameDungeon(advanced, relocated)
                    sameDungeon(relocated, member.view { it.nodeId != advanced.nodeId })
                    check(relocated.nodeId in listOf("instanceA", "instanceB"))
                    check(leader.sessionInfo() == leaderSession && member.sessionInfo() == memberSession)
                    nodes.replace(advanced.nodeId)
                    secondParty(nodes.endpoint, replacement = advanced.nodeId, first = advanced)
                    check(leader.sessionInfo() == leaderSession && member.sessionInfo() == memberSession)
                }
                else -> error("Unknown scenario: $id")
            }
        }
    }
}

private suspend fun awaitUsingClient(endpoint: String, playerId: String, action: suspend (Client) -> Unit) {
    val client = Client(endpoint, playerId)
    withCleanup(cleanup = { client.close() }) { client.connect(); action(client) }
}
