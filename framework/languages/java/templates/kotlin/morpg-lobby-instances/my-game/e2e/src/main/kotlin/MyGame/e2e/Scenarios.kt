package MyGame.e2e

import MyGame.*
import java.util.UUID

private fun equal(expected: Any?, actual: Any?) {
    check(expected == actual) { "Expected $expected, got $actual" }
}

private fun sameState(a: DungeonView, b: DungeonView) {
    equal(a.dungeonId, b.dungeonId)
    equal(a.partyId, b.partyId)
    equal(a.members, b.members)
    equal(a.stage, b.stage)
    equal(a.phase, b.phase); equal(a.runId, b.runId)
}

private suspend fun enter(a: Client, b: Client): DungeonView {
    a.connect()
    a.next<LobbyView>()
    b.connect()
    b.next<LobbyView>()
    a.clear()
    b.clear()
    a.send(JoinPartyRequest(Names.newPartyId())); equal(ActionRejected(Rules.INVALID_PARTY), a.next<ActionRejected>())
    a.send(CreatePartyRequest())
    val created = a.next<PartyView>(); val party = created.partyId
    check(Regex("party-[0-9a-f]{32}").matches(party))
    equal(PartyView(party, listOf(a.playerId), listOf(false)), created)
    a.clear()
    b.send(JoinPartyRequest(party))
    val joined = a.next<PartyView>()
    equal(joined, b.next<PartyView>())
    equal(PartyView(party, listOf(a.playerId, b.playerId), listOf(false, false)), joined)
    a.send(EnterDungeonRequest())
    equal(ActionRejected(Rules.NOT_READY), a.next<ActionRejected>())
    a.send(ViewRequest()); equal(joined, a.next<PartyView>())
    b.send(EnterDungeonRequest()); equal(ActionRejected(Rules.NOT_LEADER), b.next<ActionRejected>())
    b.send(ViewRequest()); equal(joined, b.next<PartyView>())
    b.send(JoinDungeon("dungeon-forged", "run-forged")); equal(ActionRejected(Rules.PACKET_NOT_ALLOWED), b.next<ActionRejected>())
    a.clear()
    b.clear()
    a.send(ReadyRequest())
    var ready = a.next<PartyView>()
    equal(ready, b.next<PartyView>())
    equal(listOf(true, false), ready.ready)
    a.clear()
    b.clear()
    b.send(ReadyRequest())
    ready = a.next<PartyView>()
    equal(ready, b.next<PartyView>())
    equal(listOf(true, true), ready.ready)
    a.clear()
    b.clear()
    a.send(EnterDungeonRequest())
    a.send(EnterDungeonRequest())
    val dungeon = a.next<DungeonView> { it.phase == RunPhase.Active }
    equal(dungeon, b.next<DungeonView> { it.phase == RunPhase.Active })
    check(Regex("run-[0-9a-f]{32}").matches(requireNotNull(dungeon.runId)))
    equal(Names.dungeonId(requireNotNull(dungeon.runId)), dungeon.dungeonId)
    equal(party, dungeon.partyId)
    equal(listOf(a.playerId, b.playerId), dungeon.members)
    equal(0L, dungeon.stage)
    sameState(dungeon, a.view())
    sameState(dungeon, b.view())
    return dungeon
}

private suspend fun advance(a: Client, b: Client, stage: Long): DungeonView {
    a.clear()
    b.clear()
    a.send(AdvanceRequest())
    val view = a.next<DungeonView>()
    equal(view, b.next<DungeonView>())
    equal(stage, view.stage)
    return view
}

private suspend fun complete(a: Client, b: Client) {
    val done = advance(a, b, Rules.FINAL_STAGE.toLong()); equal(RunPhase.Completed, done.phase)
    a.send(AdvanceRequest()); equal(ActionRejected(Rules.RUN_NOT_ACTIVE), a.next<ActionRejected>())
    sameState(done, a.view())
}

suspend fun scenario(scenario: Scenario, nodes: Nodes) {
    val id = UUID.randomUUID().toString()
    val clients = mutableListOf<Client>()
    fun client(player: String) = Client(nodes.endpoint, player).also { clients.add(it) }
    withCleanup(
        cleanup = {
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
    ) {
        val a = client("$id-p1")
        val b = client("$id-p2")
        val initial = enter(a, b)
        val sa = a.sessionInfo()
        val sb = b.sessionInfo()
        equal(a.playerId, sa.actorId)
        equal(b.playerId, sb.actorId)
        val before = advance(a, b, 1L)
        equal(initial.nodeId, before.nodeId)
        equal(sa, a.sessionInfo())
        equal(sb, b.sessionInfo())
        when (scenario) {
            Scenario.PARTY -> {
                val separate = enter(client("$id-second1"), client("$id-second2"))
                check(separate.dungeonId != initial.dungeonId && separate.runId != initial.runId)
                advance(a, b, 2L); complete(a, b)
            }
            Scenario.RECONNECT -> {
                b.close()
                clients.remove(b)
                val again = client(b.playerId)
                again.connect()
                val restored = again.next<DungeonView>()
                sameState(before, restored)
                equal(before.nodeId, restored.nodeId)
                val identity = again.sessionInfo()
                equal(sb.actorId, identity.actorId)
                equal(sb.objectGeneration, identity.objectGeneration)
                check(sb.sessionId != identity.sessionId) { "Reconnect reused Session" }
                advance(a, again, 2L)
                complete(a, again)
                equal(0, again.disconnects)
            }
            Scenario.UPDATE -> {
                nodes.drain(before.nodeId)
                equal(sa, a.sessionInfo())
                equal(sb, b.sessionInfo())
                val after = a.view()
                equal(after, b.view())
                sameState(before, after)
                check(before.nodeId != after.nodeId) { "Dungeon did not relocate" }
                nodes.replace(before.nodeId)
                val freshA = client("$id-new1"); val freshB = client("$id-new2")
                val fresh = enter(freshA, freshB)
                advance(freshA, freshB, 1L)
                equal(before.nodeId, fresh.nodeId)
                check(fresh.dungeonId != initial.dungeonId && fresh.runId != initial.runId)
                advance(a, b, 2L)
                complete(a, b)
                equal(sa, a.sessionInfo())
                equal(sb, b.sessionInfo())
            }
        }
        equal(0, a.disconnects)
        if (scenario != Scenario.RECONNECT) equal(0, b.disconnects)
    }
}
