package MyGame.field

import MyGame.*
import java.util.UUID

data class PartyRun(
    var members: MutableList<String> = mutableListOf(),
    var entered: MutableSet<String> = linkedSetOf(),
    var phase: RunPhase = RunPhase.Forming,
    var runId: String? = null,
) {
    companion object {
        fun validId(id: String?, prefix: String): Boolean =
            id != null && id.startsWith(prefix) && id.length == prefix.length + 32 &&
                id.substring(prefix.length).all { it in "0123456789abcdef" }
    }

    fun join(id: String): ActionRejected? {
        if (phase != RunPhase.Forming || id in members || members.size >= Rules.MAXIMUM_PARTY_MEMBERS || id.isBlank())
            return ActionRejected(Rejection.INVALID_PARTY)
        members.add(id)
        return null
    }

    fun leave(id: String) {
        if (phase == RunPhase.Forming) members.remove(id)
    }

    fun enter(id: String): ActionRejected? {
        if (members.firstOrNull() != id) return ActionRejected(Rejection.NOT_LEADER)
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = "run-${UUID.randomUUID().toString().replace("-", "")}"
            entered.clear()
            phase = RunPhase.Preparing
        }
        return null
    }

    fun prepared(id: String): ActionRejected? {
        if (runId != id || phase !in setOf(RunPhase.Preparing, RunPhase.Entering))
            return ActionRejected(Rejection.RUN_MISMATCH)
        phase = RunPhase.Entering
        return null
    }

    fun arrived(id: String, actorId: String): ActionRejected? {
        if (runId != id || actorId !in members || phase !in setOf(RunPhase.Entering, RunPhase.Active))
            return ActionRejected(Rejection.RUN_MISMATCH)
        entered.add(actorId)
        return null
    }

    fun started(id: String): ActionRejected? {
        if (runId != id || phase !in setOf(RunPhase.Entering, RunPhase.Active) || entered.size != members.size)
            return ActionRejected(Rejection.RUN_MISMATCH)
        phase = RunPhase.Active
        return null
    }

    fun completed(id: String): ActionRejected? {
        if (runId != id || phase !in setOf(RunPhase.Active, RunPhase.Completed))
            return ActionRejected(Rejection.RUN_MISMATCH)
        phase = RunPhase.Completed
        return null
    }

    fun view(partyId: String) =
        PartyView(partyId, members.toList(), members.firstOrNull() ?: "", phase, runId)
}
