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
}

private suspend fun enter(a: Client, b: Client, party: String): DungeonView {
    a.connect()
    a.next<LobbyView>()
    b.connect()
    b.next<LobbyView>()
    a.clear()
    b.clear()
    a.send(CreatePartyRequest(party))
    equal(PartyView(party, listOf(a.playerId), listOf(false)), a.next<PartyView>())
    a.clear()
    b.send(JoinPartyRequest(party))
    val joined = a.next<PartyView>()
    equal(joined, b.next<PartyView>())
    equal(PartyView(party, listOf(a.playerId, b.playerId), listOf(false, false)), joined)
    a.send(EnterDungeonRequest())
    equal(Rules.NOT_READY, a.next<ActionRejected>().reason)
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
    val dungeon = a.next<DungeonView>()
    equal(dungeon, b.next<DungeonView>())
    equal(Names.DUNGEON_PREFIX + party, dungeon.dungeonId)
    equal(party, dungeon.partyId)
    equal(listOf(a.playerId, b.playerId), dungeon.members)
    equal(0, dungeon.stage)
    sameState(dungeon, a.view())
    sameState(dungeon, b.view())
    return dungeon
}

private suspend fun advance(a: Client, b: Client, stage: Int): DungeonView {
    a.clear()
    b.clear()
    a.send(AdvanceRequest())
    val view = a.next<DungeonView>()
    equal(view, b.next<DungeonView>())
    equal(stage, view.stage)
    return view
}

suspend fun scenario(scenario: Scenario, nodes: Nodes) {
    val id = UUID.randomUUID().toString()
    val clients = mutableListOf<Client>()
    fun client(player: String) = Client(nodes.endpoint, player).also { clients.add(it) }
    try {
        val a = client("$id-p1")
        val b = client("$id-p2")
        val initial = enter(a, b, id)
        val sa = a.sessionInfo()
        val sb = b.sessionInfo()
        equal(a.playerId, sa.actorId)
        equal(b.playerId, sb.actorId)
        val before = advance(a, b, 1)
        equal(initial.nodeId, before.nodeId)
        equal(sa, a.sessionInfo())
        equal(sb, b.sessionInfo())
        when (scenario) {
            Scenario.PARTY -> Unit
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
                advance(a, again, 2)
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
                val fresh = enter(client("$id-new1"), client("$id-new2"), "$id-replacement")
                equal(before.nodeId, fresh.nodeId)
                advance(a, b, 2)
                equal(sa, a.sessionInfo())
                equal(sb, b.sessionInfo())
            }
        }
        equal(0, a.disconnects)
        if (scenario != Scenario.RECONNECT) equal(0, b.disconnects)
    } finally {
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
}
