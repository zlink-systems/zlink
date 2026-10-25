package MyGame.instance.domain
import MyGame.*
class DungeonState {
    var partyId = ""
    var runId: String? = null
    var members: List<String> = emptyList()
    var joined: MutableSet<String> = linkedSetOf()
    var stage = 0L
    var phase = RunPhase.Preparing
    fun prepare(r: PrepareDungeon): String? {
        if (runId != null) return if (runId == r.runId && partyId == r.partyId && members == r.members) null else Rules.RUN_MISMATCH
        if (r.runId.isBlank() || r.partyId.isBlank() || r.members.isEmpty() || r.members.size > Rules.MAX_MEMBERS || r.members.distinct().size != r.members.size || r.members.any { it.isBlank() }) return Rules.RUN_MISMATCH
        runId = r.runId; partyId = r.partyId; members = r.members.toList(); phase = RunPhase.Entering; return null
    }
    fun admit(id: String, r: JoinDungeon, dungeonId: String) = r.dungeonId == dungeonId && r.runId == runId && id in members
    fun join(id: String): Boolean {
        if (id !in members) return false
        joined.add(id)
        if (phase != RunPhase.Entering || joined.size != members.size) return false
        phase = RunPhase.Active; return true
    }
    fun advance(): String? {
        if (stage >= Rules.MAXIMUM_INTEGER || stage < 0) return Rules.INTEGER_OUT_OF_RANGE
        if (phase != RunPhase.Active) return Rules.RUN_NOT_ACTIVE
        stage++; if (stage == Rules.FINAL_STAGE.toLong()) phase = RunPhase.Completed
        return null
    }
}
