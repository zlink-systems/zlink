package MyGame.instance

import MyGame.*

data class DungeonRun(
    var partyId: String = "",
    var runId: String = "",
    var members: MutableList<String> = mutableListOf(),
    var entered: MutableSet<String> = linkedSetOf(),
    var phase: RunPhase = RunPhase.Preparing,
    var stage: Int = 0,
) {
    fun prepare(request: PrepareDungeon): ActionRejected? {
        if (runId.isNotEmpty()) {
            return if (runId == request.runId && partyId == request.partyId && members == request.members)
                null
            else ActionRejected(Rejection.RUN_MISMATCH)
        }
        if (
            request.members.isEmpty() ||
                request.members.size > Rules.MAXIMUM_PARTY_MEMBERS ||
                request.members.distinct().size != request.members.size ||
                request.members.any { it.isBlank() }
        ) return ActionRejected(Rejection.INVALID_PARTY)
        runId = request.runId
        partyId = request.partyId
        members = request.members.toMutableList()
        phase = RunPhase.Entering
        return null
    }

    fun canJoin(actorId: String, requestedRunId: String) =
        runId == requestedRunId && actorId in members

    fun arrived(actorId: String) {
        entered.add(actorId)
        if (entered.size == members.size && phase == RunPhase.Entering) phase = RunPhase.Active
    }

    fun advance(): ActionRejected? {
        if (phase != RunPhase.Active) return ActionRejected(Rejection.RUN_NOT_ACTIVE)
        stage = Math.incrementExact(stage)
        if (stage == Rules.FINAL_STAGE) phase = RunPhase.Completed
        return null
    }
}
