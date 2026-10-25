package MyGame

import java.util.UUID

data class PartyRun(
    var partyId: String = "party-" + UUID.randomUUID().toString().replace("-", ""),
    var members: MutableList<String> = mutableListOf(),
    var phase: RunPhase = RunPhase.Forming,
    var runId: String? = null,
) {
    fun enter(player: String): String? {
        if (members.firstOrNull() != player) return Rejection.NOT_LEADER
        if (phase == RunPhase.Forming || phase == RunPhase.Completed) {
            runId = "run-" + UUID.randomUUID().toString().replace("-", "")
            phase = RunPhase.Preparing
        }
        return null
    }

    fun prepared(runId: String): String? {
        if (this.runId != runId || (phase != RunPhase.Preparing && phase != RunPhase.Entering))
            return Rejection.RUN_MISMATCH
        phase = RunPhase.Entering
        return null
    }

    fun started(runId: String): String? {
        if (this.runId != runId || (phase != RunPhase.Entering && phase != RunPhase.Active))
            return Rejection.RUN_MISMATCH
        phase = RunPhase.Active
        return null
    }

    fun completed(runId: String): String? {
        if (this.runId != runId || (phase != RunPhase.Active && phase != RunPhase.Completed))
            return Rejection.RUN_MISMATCH
        phase = RunPhase.Completed
        return null
    }

    fun leave(player: String) {
        if (phase == RunPhase.Forming) members.remove(player)
    }

    fun changeChannel(): String? =
        if (phase == RunPhase.Forming) null else Rejection.RUN_IN_PROGRESS

    fun view() = PartyView(partyId, members.toList(), phase, runId)
}

data class DungeonState(
    var partyId: String = "",
    var runId: String = "",
    var ownerSpotId: String = "",
    var members: List<String> = emptyList(),
    var joined: MutableSet<String> = linkedSetOf(),
    var phase: RunPhase = RunPhase.Forming,
    var stage: Long = 0,
) {
    fun prepare(dungeonId: String, request: PrepareDungeon): String? {
        if (runId.isNotEmpty())
            return if (
                runId == request.runId &&
                    partyId == request.partyId &&
                    ownerSpotId == request.ownerSpotId &&
                    members == request.members
            ) null else Rejection.RUN_MISMATCH

        if (
            !Names.validId(request.runId, "run-") ||
                !Names.validId(request.partyId, "party-") ||
                request.ownerSpotId.isBlank() ||
                Names.dungeonId(request.runId) != dungeonId ||
                request.members.size !in 1..Names.MAXIMUM_PARTY_MEMBERS ||
                request.members.distinct().size != request.members.size ||
                request.members.any { it.isBlank() }
        ) return Rejection.RUN_MISMATCH

        runId = request.runId
        partyId = request.partyId
        ownerSpotId = request.ownerSpotId
        members = request.members.toList()
        phase = RunPhase.Entering
        return null
    }

    fun accepts(actor: String, dungeonId: String, request: JoinDungeon): Boolean =
        request.dungeonId == dungeonId && request.runId == runId && actor in members

    fun join(actor: String): Boolean {
        if (actor !in members) return false
        joined.add(actor)
        if (phase != RunPhase.Entering || joined.size != members.size) return false
        phase = RunPhase.Active
        return true
    }

    fun advance(): String? {
        if (stage < 0 || stage >= Names.MAXIMUM_INTEGER) return Rejection.INTEGER_OUT_OF_RANGE
        if (phase != RunPhase.Active) return Rejection.RUN_NOT_ACTIVE
        stage++
        if (stage == Names.FINAL_STAGE) phase = RunPhase.Completed
        return null
    }
}

object ChannelRules {
    fun validate(channel: Long): String? =
        if (channel > Names.MAXIMUM_INTEGER || channel < -Names.MAXIMUM_INTEGER)
            Rejection.INTEGER_OUT_OF_RANGE
        else if (channel < 1 || channel > Names.CHANNEL_COUNT.toLong()) Rejection.INVALID_CHANNEL
        else null
}
