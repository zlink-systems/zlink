package MyGame.lobby.domain

import MyGame.*

class PartyState {
    var partyId: String = ""
    var phase = RunPhase.Forming
    var runId: String? = null
    var entered: MutableSet<String> = linkedSetOf()
    var members: MutableList<String> = mutableListOf()
    var ready: MutableSet<String> = linkedSetOf()

    fun join(id: String, request: PartyJoin): Boolean {
        if (
            phase != RunPhase.Forming || request.partyId.isBlank() ||
                id in members ||
                members.size >= Rules.MAX_MEMBERS ||
                (if (request.create) members.isNotEmpty()
                else partyId != request.partyId || members.isEmpty())
        )
            return false
        partyId = request.partyId
        members.add(id)
        return true
    }

    fun leave(id: String) {
        if (phase != RunPhase.Forming) return
        members.remove(id)
        ready.remove(id)
    }

    fun view() = PartyView(partyId, members.toList(), members.map { it in ready }, phase, runId)

    fun entryRejection(id: String): String? =
        when {
            members.firstOrNull() != id -> Rules.NOT_LEADER
            members.any { it !in ready } -> Rules.NOT_READY
            else -> null
        }
    fun markReady(id: String) { if (phase == RunPhase.Forming && id in members) ready.add(id) }
    fun enter(id: String): String? {
        entryRejection(id)?.let { return it }
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = Names.newRunId(); phase = RunPhase.Preparing; entered.clear()
        }
        return null
    }
    fun arrived(run: String, player: String): String? {
        if (runId != run || phase == RunPhase.Forming || player !in members) return Rules.RUN_MISMATCH
        entered.add(player); return null
    }
    fun prepared(id: String): String? {
        if (runId != id || phase !in listOf(RunPhase.Preparing, RunPhase.Entering)) return Rules.RUN_MISMATCH
        phase = RunPhase.Entering; return null
    }
    fun started(id: String): String? {
        if (runId != id || phase !in listOf(RunPhase.Entering, RunPhase.Active)) return Rules.RUN_MISMATCH
        phase = RunPhase.Active; return null
    }
    fun completed(id: String): String? {
        if (runId != id || phase !in listOf(RunPhase.Active, RunPhase.Completed)) return Rules.RUN_MISMATCH
        phase = RunPhase.Completed; return null
    }
}
