package MyGame.e2e

import MyGame.*

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
            before.stage == after.stage
    ) {
        "Dungeon state changed"
    }
}

private fun party(view: FieldView, id: String, members: List<String>) =
    view.parties.any { it.partyId == id && it.members == members }

class Scenarios(private val nodes: Nodes) {
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

    private suspend fun enter(p1: Client, p2: Client, partyId: String): DungeonView {
        p1.send(CreatePartyRequest(partyId))
        p1.next<FieldView> { party(it, partyId, listOf(p1.playerId)) }
        val members = listOf(p1.playerId, p2.playerId)
        p2.send(JoinPartyRequest(partyId))
        p2.next<FieldView> { party(it, partyId, members) }
        p1.next<FieldView> { party(it, partyId, members) }
        p1.send(EnterDungeonRequest())
        val a = p1.next<DungeonView> { it.partyId == partyId }
        val b = p2.next<DungeonView> { it.partyId == partyId }
        sameDungeon(a, b)
        check(
            a.dungeonId == Rules.dungeonId(partyId) &&
                a.nodeId == b.nodeId &&
                a.stage == 0 &&
                a.members == members
        ) {
            "Party did not enter the same fresh dungeon"
        }
        // Query both current Spots before submitting the next stage transition.
        sameDungeon(a, p1.dungeon())
        sameDungeon(b, p2.dungeon())
        return a
    }

    private suspend fun prepare() {
        if (advanced != null) return
        leader = create("dungeon-p1")
        member = create("dungeon-p2")
        leader.join(250)
        member.join(250)
        val s1 = leader.sessionInfo()
        val s2 = member.sessionInfo()
        val initial = enter(leader, member, "party-1")
        leader.send(AdvanceRequest())
        val state = leader.next<DungeonView> { it.stage == 1 }
        sameDungeon(state, member.next<DungeonView> { it.stage == 1 })
        check(initial.nodeId == state.nodeId) { "Advance changed host" }
        check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
            "Dungeon entry changed Session"
        }
        advanced = state
    }

    suspend fun run(name: String) {
        if (name == Names.FIELD_SCENARIO) {
            val player = create("p1")
            val observer = create("border-p2")
            val observed = observer.join(199)
            val origin = player.join(198)
            check(
                origin.fieldId == "field-2" &&
                    origin.nodeId == "fieldA" &&
                    observed.nodeId == "fieldA"
            ) {
                "Field crossing origin mismatch"
            }
            val session = player.sessionInfo()
            val after = player.move(200, 1)
            check(
                after.fieldId == "field-3" &&
                    after.nodeId == "fieldB" &&
                    after.x == 200 &&
                    after.sequence == 1
            ) {
                "Field crossing destination mismatch"
            }
            val border =
                observer.next<BorderVisible> { it.playerId == player.playerId && it.sequence == 1 }
            check(
                border.observerFieldId == "field-2" &&
                    border.sourceFieldId == "field-3" &&
                    border.x == 200
            ) {
                "Border publication mismatch"
            }
            check(
                session == player.sessionInfo() &&
                    player.disconnects == 0 &&
                    observer.disconnects == 0
            ) {
                "Field crossing interrupted connection"
            }
            return
        }
        prepare()
        val state = requireNotNull(advanced)
        when (name) {
            Names.DUNGEON_SCENARIO -> {
                sameDungeon(state, leader.dungeon())
                sameDungeon(state, member.dungeon())
            }
            Names.RECONNECT -> {
                val before = member.sessionInfo()
                member.close()
                clients.remove(member)
                member = create(before.actorId)
                member.send(JoinWorldRequest(member.playerId, 250))
                val restored = member.next<DungeonView>()
                sameDungeon(state, restored)
                check(state.nodeId == restored.nodeId) { "Reconnect changed host" }
                val after = member.sessionInfo()
                sameActor(before, after)
                check(before.sessionId != after.sessionId) { "Reconnect reused physical Session" }
            }
            Names.UPDATE -> {
                val s1 = leader.sessionInfo()
                val s2 = member.sessionInfo()
                nodes.drain(state.nodeId)
                val relocated = leader.dungeon(state.nodeId)
                val other = member.dungeon(state.nodeId)
                sameDungeon(state, relocated)
                sameDungeon(relocated, other)
                check(relocated.nodeId != state.nodeId && relocated.nodeId == other.nodeId) {
                    "Dungeon did not relocate together"
                }
                check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
                    "Host relocation changed Session"
                }
                nodes.replace(state.nodeId)
                sameDungeon(relocated, leader.dungeon())
                sameDungeon(relocated, member.dungeon())
                val p3 = create("replacement-p3")
                val p4 = create("replacement-p4")
                p3.join(250)
                p4.join(250)
                check(enter(p3, p4, "party-2").nodeId == state.nodeId) {
                    "Replacement did not accept new work"
                }
                check(s1 == leader.sessionInfo() && s2 == member.sessionInfo()) {
                    "Replacement interrupted Session"
                }
            }
            else -> error("Unknown scenario: $name")
        }
        for (client in clients) check(client.disconnects == 0) {
            "Client disconnected: ${client.playerId}"
        }
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
}
