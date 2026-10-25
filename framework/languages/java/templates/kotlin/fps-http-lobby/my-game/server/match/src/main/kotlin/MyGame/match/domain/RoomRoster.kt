package MyGame.match.domain

import MyGame.Game
import MyGame.PrepareRoom
import MyGame.RoomPrepared

class RoomRoster {
    private var matchId: String? = null
    private var members: Set<String> = emptySet()

    fun prepare(roomId: String, request: PrepareRoom): RoomPrepared {
        if (
            request.matchId != roomId ||
                roomId.isBlank() ||
                request.members.size != Game.PLAYERS_PER_ROOM ||
                request.members.any(String::isBlank) ||
                request.members.toSet().size != request.members.size
        ) {
            throw IllegalArgumentException("Invalid room roster preparation.")
        }
        val existingMatchId = matchId
        if (existingMatchId != null) {
            if (existingMatchId != request.matchId || members != request.members.toSet()) {
                throw IllegalStateException("A prepared Room roster cannot be replaced.")
            }
            return RoomPrepared(existingMatchId)
        }
        matchId = request.matchId
        members = request.members.toSet()
        return RoomPrepared(request.matchId)
    }

    fun canAdmit(playerId: String): Boolean = matchId != null && playerId in members

    fun capture() = RosterSnapshot(matchId, members.sorted())

    fun restore(snapshot: RosterSnapshot) {
        check(matchId == null) { "Cannot restore an already prepared Room roster." }
        val restoredMatchId = snapshot.matchId
        if (restoredMatchId != null) {
            prepare(restoredMatchId, PrepareRoom(restoredMatchId, snapshot.members))
        } else if (snapshot.members.isNotEmpty()) {
            throw IllegalArgumentException("Unprepared Room snapshot has members.")
        }
    }
}

data class RosterSnapshot(val matchId: String?, val members: List<String>)

data class HttpRoomSnapshot(val game: RoomSnapshot, val roster: RosterSnapshot)
