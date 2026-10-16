package MyGame.e2e

import MyGame.*
import java.util.UUID

private fun sameDungeon(before: DungeonView, after: DungeonView) {
    check(before.copy(nodeId = after.nodeId) == after) {
        "Dungeon state changed: $before -> $after"
    }
}

private suspend fun enter(a: Client, b: Client, partyId: String): DungeonView {
    a.send(CreatePartyRequest(partyId))
    a.next<TownView> {
        it.parties.any { party -> party.partyId == partyId && party.members == listOf(a.playerId) }
    }
    b.send(JoinPartyRequest(partyId))
    val party: (TownView) -> Boolean = {
        it.parties.any { party ->
            party.partyId == partyId && party.members == listOf(a.playerId, b.playerId)
        }
    }
    check(a.next(party) == b.next<TownView>(party)) { "Party views differ" }
    a.send(EnterDungeonRequest())
    val dungeon = a.next<DungeonView> { it.partyId == partyId }
    check(dungeon == b.next<DungeonView> { it.partyId == partyId }) { "Dungeon views differ" }
    check(
        dungeon.dungeonId == Names.dungeonId(partyId) &&
            dungeon.stage == 0 &&
            dungeon.members == listOf(a.playerId, b.playerId)
    )
    // Current-Spot replies confirm both deferred joins completed before Advance.
    sameDungeon(dungeon, a.view { it == dungeon })
    sameDungeon(dungeon, b.view { it == dungeon })
    return dungeon
}

private suspend fun newDungeon(endpoint: String, replacement: String) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(endpoint, "new-a-$suffix")
    val b = Client(endpoint, "new-b-$suffix")
    try {
        a.connect()
        b.connect()
        a.join()
        a.next<TownView> { it.townId == Names.townId(2) }
        b.join()
        b.next<TownView> { it.players.containsAll(listOf(a.playerId, b.playerId)) }
        check(enter(a, b, "new-party-$suffix").nodeId == replacement) {
            "Replacement did not accept new work"
        }
    } finally {
        try {
            b.close()
        } finally {
            a.close()
        }
    }
}

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val a = Client(nodes.endpoint, "p1-$suffix")
    val b = if (id == "channel") null else Client(nodes.endpoint, "p2-$suffix")
    var bClosed = false
    try {
        a.connect()
        if (id == "channel") {
            a.join(1)
            check(a.next<TownView> { it.townId == Names.townId(1) }.nodeId == "townA")
            val before = a.sessionInfo()
            a.send(ChangeChannelRequest(2))
            val changed = a.next<TownView> { it.townId == Names.townId(2) }
            check(changed.nodeId == "townB" && a.playerId in changed.players)
            check(a.sessionInfo() == before) { "Channel change altered Session binding" }
        } else {
            requireNotNull(b)
            b.connect()
            a.join()
            a.next<TownView> { it.townId == Names.townId(2) }
            b.join()
            b.next<TownView> { it.players.containsAll(listOf(a.playerId, b.playerId)) }
            val session1 = a.sessionInfo()
            val session2 = b.sessionInfo()
            val dungeon = enter(a, b, "party-$suffix")
            a.send(AdvanceRequest())
            val before = a.next<DungeonView> { it.stage == 1 }
            check(
                before == b.next<DungeonView> { it.stage == 1 } && before.nodeId == dungeon.nodeId
            )
            check(a.sessionInfo() == session1 && b.sessionInfo() == session2)
            when (id) {
                "dungeon" -> Unit
                "reconnect" -> {
                    bClosed = true
                    b.close()
                    val again = Client(nodes.endpoint, b.playerId)
                    try {
                        again.connect()
                        again.join()
                        val restored = again.next<DungeonView>()
                        check(restored == before) { "Reconnect changed dungeon state" }
                        val identity = again.sessionInfo()
                        check(
                            identity.sessionId != session2.sessionId &&
                                identity.actorId == session2.actorId &&
                                identity.objectGeneration == session2.objectGeneration
                        )
                    } finally {
                        again.close()
                    }
                }
                "update" -> {
                    nodes.drain(before.nodeId)
                    val relocated = a.view { it.nodeId != before.nodeId }
                    sameDungeon(before, relocated)
                    check(relocated == b.view { it.nodeId != before.nodeId })
                    check(relocated.nodeId in listOf("instanceA", "instanceB"))
                    check(a.sessionInfo() == session1 && b.sessionInfo() == session2)
                    check(
                        a.disconnects == 0 &&
                            b.disconnects == 0 &&
                            a.connector.isConnected &&
                            b.connector.isConnected
                    )
                    nodes.replace(before.nodeId)
                    newDungeon(nodes.endpoint, before.nodeId)
                    check(a.sessionInfo() == session1 && b.sessionInfo() == session2)
                }
                else -> error("Unknown scenario: $id")
            }
            if (!bClosed) check(b.disconnects == 0 && b.connector.isConnected)
        }
        check(a.disconnects == 0 && a.connector.isConnected)
    } finally {
        try {
            if (!bClosed) b?.close()
        } finally {
            a.close()
        }
    }
}
