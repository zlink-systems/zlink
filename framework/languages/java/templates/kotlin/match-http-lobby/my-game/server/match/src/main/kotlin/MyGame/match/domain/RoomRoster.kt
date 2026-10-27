package MyGame.match.domain

import MyGame.*

class RoomRoster {
    private var matchId: String? = null
    private var members: Set<String> = emptySet()

    fun prepare(roomId: String, request: PrepareRoom): RoomPrepared {
        require(
            roomId.isNotBlank() &&
                roomId == request.matchId &&
                request.members.size == Rules.PLAYERS &&
                request.members.all { it.isNotBlank() } &&
                request.members.toSet().size == request.members.size
        ) {
            "Invalid Room roster preparation"
        }
        val requestedMembers = request.members.toSet()
        if (matchId != null) {
            check(matchId == request.matchId && members == requestedMembers) {
                "A prepared Room roster cannot be replaced"
            }
            return RoomPrepared(matchId!!)
        }
        matchId = request.matchId
        members = requestedMembers
        return RoomPrepared(request.matchId)
    }

    fun canAdmit(playerId: String): Boolean = matchId != null && playerId in members

    fun capture() = RosterSnapshot(matchId, members.sorted())

    fun restore(snapshot: RosterSnapshot) {
        check(matchId == null) { "Cannot restore a prepared Room roster" }
        require(snapshot.matchId != null || snapshot.members.isEmpty()) {
            "Unprepared Room snapshot has members"
        }
        if (snapshot.matchId != null) {
            prepare(snapshot.matchId, PrepareRoom(snapshot.matchId, snapshot.members))
        }
    }

    data class RosterSnapshot(val matchId: String? = null, val members: List<String> = emptyList())
}
