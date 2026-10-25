package MyGame.e2e

import MyGame.*
import kotlinx.coroutines.delay

private fun sameActor(before: SessionInfo, after: SessionInfo) {
    check(before.actorId == after.actorId && before.objectGeneration == after.objectGeneration) {
        "Actor identity changed"
    }
}

private fun sameDungeon(before: DungeonView, after: DungeonView) {
    check(
        before.dungeonId == after.dungeonId &&
            before.partyId == after.partyId &&
            before.members == after.members &&
            before.stage == after.stage &&
            before.runId == after.runId &&
            before.phase == after.phase,
    ) {
        "Dungeon state changed"
    }
}

private fun party(view: PartyView, id: String, members: List<String>) =
    view.partyId == id && view.members == members

class Scenarios(private val nodes: Nodes, selectedScenarios: List<String>) {
    private val selectedScenarios = selectedScenarios.toSet()
    private val clients = mutableListOf<Client>()
    private lateinit var leader: Client
    private lateinit var member: Client
    private var advanced: DungeonView? = null

    private suspend fun create(id: String): Client {
        val client = Client(nodes.endpoint, id)
        clients.add(client)
        client.connect()
        return client
    }

    private suspend fun enter(p1: Client, p2: Client): DungeonView {
        p1.send(CreatePartyRequest())
        val created =
            p1.next<PartyView> { it.members == listOf(p1.playerId) && it.phase == RunPhase.Forming }
        val partyId = created.partyId
        check(MyGame.field.PartyRun.validId(partyId, "party-")) { "Party ID is not server-issued" }

        p2.send(JoinPartyRequest(partyId))
        val members = listOf(p1.playerId, p2.playerId)
        val joined = p2.next<PartyView> { party(it, partyId, members) }
        p1.next<PartyView> { party(it, partyId, members) }

        p2.send(EnterDungeonRequest())
        val nonleader = p2.next<ActionRejected> { it.reason == Rejection.NOT_LEADER }
        check(nonleader.retryAfterMs == 0) { "Leadership rejection has a cooldown" }
        p2.send(ViewRequest())
        val unchanged = p2.next<PartyView> { it.partyId == partyId }
        check(
            joined.members == unchanged.members &&
                joined.phase == unchanged.phase &&
                unchanged.runId == null,
        ) {
            "Rejected entry changed party state"
        }

        p1.send(EnterDungeonRequest())
        p1.send(EnterDungeonRequest())
        val leaderView = p1.next<DungeonView> { it.partyId == partyId && it.phase == RunPhase.Active }
        val memberView = p2.next<DungeonView> { it.partyId == partyId && it.phase == RunPhase.Active }
        sameDungeon(leaderView, memberView)
        check(
            leaderView.dungeonId == Rules.dungeonId(leaderView.runId) &&
                leaderView.nodeId == memberView.nodeId &&
                leaderView.stage == 0 &&
                MyGame.field.PartyRun.validId(leaderView.runId, "run-") &&
                leaderView.members == members,
        ) {
            "Party did not enter the same fresh dungeon"
        }
        // Replies from both current Spots confirm deferred joins finished before Advance.
        sameDungeon(leaderView, p1.dungeon())
        sameDungeon(memberView, p2.dungeon())
        return leaderView
    }

    private suspend fun prepare() {
        if (advanced != null) return
        leader = create("dungeon-p1")
        member = create("dungeon-p2")
        leader.join(250.0)
        member.join(250.0)
        val s1 = leader.sessionInfo()
        val s2 = member.sessionInfo()
        val initial = enter(leader, member)
        leader.send(AdvanceRequest())
        advanced = leader.next<DungeonView> { it.stage == 1 }
        sameDungeon(requireNotNull(advanced), member.next<DungeonView> { it.stage == 1 })
        check(initial.nodeId == requireNotNull(advanced).nodeId) { "Advance changed host" }
        check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
            "Dungeon entry changed Session"
        }
    }

    suspend fun run(name: String) {
        if (name == Names.FIELD_SCENARIO) {
            val player = create("p1")
            val observer = create("border-p2")
            val observed = observer.join(199.0)
            val origin = player.join(198.0)
            check(
                origin.fieldId == "field-2" &&
                    origin.nodeId == "fieldA" &&
                    observed.nodeId == "fieldA",
            ) {
                "Field crossing origin mismatch"
            }
            player.send(JoinPartyRequest("party-${"0".repeat(32)}"))
            player.next<ActionRejected> {
                it.reason == Rejection.INVALID_PARTY && it.retryAfterMs == 0
            }
            player.send(CreatePartyRequest())
            val partyView = player.next<PartyView> { it.members == listOf(player.playerId) }
            observer.send(JoinPartyRequest(partyView.partyId))
            val members = listOf(player.playerId, observer.playerId)
            observer.next<PartyView> { party(it, partyView.partyId, members) }
            player.next<PartyView> { party(it, partyView.partyId, members) }

            player.send(JoinDungeon("internal", "internal"))
            player.next<ActionRejected> {
                it.reason == Rejection.PACKET_NOT_ALLOWED && it.retryAfterMs == 0
            }
            val session = player.sessionInfo()
            player.send(MoveRequest(2.0, 1L))
            val after = player.next<WorldState> { it.sequence == 1L }
            check(
                after.fieldId == "field-3" &&
                    after.nodeId == "fieldB" &&
                    after.x == 200.0 &&
                    after.sequence == 1L,
            ) {
                "Field crossing destination mismatch"
            }
            val border =
                observer.next<BorderVisible> { it.playerId == player.playerId && it.sequence == 1L }
            check(
                border.observerFieldId == "field-2" &&
                    border.sourceFieldId == "field-3" &&
                    border.x == 200.0,
            ) {
                "Border publication mismatch"
            }
            check(
                session == player.sessionInfo() &&
                    player.disconnects == 0 &&
                    observer.disconnects == 0,
            ) {
                "Field crossing interrupted connection"
            }

            player.send(ViewRequest())
            val playerParty = player.next<PartyView> { it.partyId == partyView.partyId }
            observer.send(ViewRequest())
            val observerParty = observer.next<PartyView> { it.partyId == partyView.partyId }
            check(
                playerParty.members == members &&
                    observerParty.members == members &&
                    playerParty.leaderId == player.playerId &&
                    observerParty.leaderId == player.playerId,
            ) {
                "Cross-field movement changed party membership or leadership"
            }

            for (entry in listOf(
                MoveCheck(MoveRequest(11.0, 2L), Rejection.STEP_TOO_LARGE),
                MoveCheck(MoveRequest(0.0, Rules.MAX_SAFE_INTEGER + 1), Rejection.INTEGER_OUT_OF_RANGE),
                MoveCheck(MoveRequest(0.0, 1L), Rejection.STALE_SEQUENCE),
            )) {
                player.send(entry.request)
                val rejected =
                    player.next<ActionRejected> {
                        it.reason == entry.reason && it.retryAfterMs == 0
                    }
                check(rejected.reason == entry.reason) { "Missing movement rejection: ${entry.reason}" }
                player.send(ViewRequest())
                val state = player.next<WorldState>()
                check(state.x == after.x && state.sequence == after.sequence) {
                    "Rejected movement changed state"
                }
            }

            player.send(MoveRequest(0.0, 2L))
            player.send(MoveRequest(10.0, 3L))
            val accepted = player.next<WorldState> { it.sequence == 2L }
            val budget = player.next<ActionRejected> { it.reason == Rejection.MOVE_BUDGET }
            player.send(ViewRequest())
            val budgetState = player.next<WorldState> { it.sequence == 2L }
            check(
                accepted.x == after.x &&
                    budgetState.x == after.x &&
                    budgetState.sequence == 2L &&
                    budget.retryAfterMs > 0,
            ) {
                "Movement budget rejection changed state: accepted=$accepted, rejection=$budget, " +
                    "current=$budgetState, before=$after"
            }
            delay(budget.retryAfterMs.toLong())
            val retried = player.move(10.0, 3L)
            check(retried.x == 210.0 && retried.sequence == 3L) {
                "Move after server retry delay failed"
            }
            return
        }

        prepare()
        when (name) {
            Names.DUNGEON_SCENARIO -> {
                val initial = requireNotNull(advanced)
                leader.send(EnterDungeonRequest())
                sameDungeon(initial, leader.next<DungeonView> { it.dungeonId == initial.dungeonId })
                member.send(CreatePartyRequest())
                member.next<ActionRejected> { it.reason == Rejection.RUN_IN_PROGRESS }
                member.send(JoinPartyRequest("party-${"0".repeat(32)}"))
                member.next<ActionRejected> { it.reason == Rejection.RUN_IN_PROGRESS }
                for (stage in 2..Rules.FINAL_STAGE) {
                    leader.send(AdvanceRequest())
                    advanced = leader.next<DungeonView> { it.stage == stage }
                    sameDungeon(requireNotNull(advanced), member.next<DungeonView> { it.stage == stage })
                }
                val completed = requireNotNull(advanced)
                check(completed.phase == RunPhase.Completed) { "Final stage did not complete run" }
                val completedParty =
                    leader.next<PartyView> {
                        it.phase == RunPhase.Completed && it.runId == completed.runId
                    }
                check(completedParty.partyId == completed.partyId) {
                    "Completed run was not reported to its Party Spot"
                }
                leader.send(AdvanceRequest())
                leader.next<ActionRejected> { it.reason == Rejection.RUN_NOT_ACTIVE }
                sameDungeon(completed, leader.dungeon())
                if (Names.UPDATE !in selectedScenarios) {
                    val otherLeader = create("other-leader")
                    val otherMember = create("other-member")
                    otherLeader.join(250.0)
                    otherMember.join(250.0)
                    val other = enter(otherLeader, otherMember)
                    check(other.dungeonId != completed.dungeonId && other.runId != completed.runId) {
                        "Different parties shared a dungeon run"
                    }
                }
            }
            Names.RECONNECT -> {
                val current = requireNotNull(advanced)
                val before = member.sessionInfo()
                member.close()
                clients.remove(member)
                member = create(before.actorId)
                member.send(JoinWorldRequest(member.playerId, 250.0))
                val restored = member.next<DungeonView>()
                sameDungeon(current, restored)
                check(current.nodeId == restored.nodeId) { "Reconnect changed host" }
                val after = member.sessionInfo()
                sameActor(before, after)
                check(before.sessionId != after.sessionId) { "Reconnect reused physical Session" }
            }
            Names.UPDATE -> {
                val current = requireNotNull(advanced)
                val s1 = leader.sessionInfo()
                val s2 = member.sessionInfo()
                nodes.drain(current.nodeId)
                val relocated = leader.dungeon(current.nodeId)
                val other = member.dungeon(current.nodeId)
                sameDungeon(current, relocated)
                sameDungeon(relocated, other)
                check(relocated.nodeId != current.nodeId && relocated.nodeId == other.nodeId) {
                    "Dungeon did not relocate together"
                }
                check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
                    "Host relocation changed Session"
                }
                nodes.replace(current.nodeId)
                sameDungeon(relocated, leader.dungeon())
                sameDungeon(relocated, member.dungeon())
                val p3 = create("replacement-p3")
                val p4 = create("replacement-p4")
                p3.join(250.0)
                p4.join(250.0)
                val fresh = enter(p3, p4)
                check(
                    fresh.nodeId == current.nodeId &&
                        fresh.dungeonId != current.dungeonId &&
                        fresh.runId != current.runId,
                ) {
                    "Replacement did not accept new work"
                }
                check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
                    "Replacement interrupted Session"
                }
            }
            else -> error("Unknown scenario: $name")
        }
        for (client in clients) check(client.disconnects == 0) { "Client disconnected: ${client.playerId}" }
    }

    suspend fun close() {
        var failure: Exception? = null
        for (client in clients.asReversed()) {
            try {
                client.close()
            } catch (error: Exception) {
                if (failure == null) failure = error else failure.addSuppressed(error)
            }
        }
        failure?.let { throw it }
    }

    private data class MoveCheck(val request: MoveRequest, val reason: String)
}
