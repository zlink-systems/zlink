package MyGame.lobby.domain

import MyGame.*

class PartyState {
    var partyId: String = ""
    var members: MutableList<String> = mutableListOf()
    var ready: MutableSet<String> = linkedSetOf()

    fun join(id: String, request: PartyJoin): Boolean {
        if (
            request.partyId.isBlank() ||
                id in members ||
                members.size >= Rules.MAX_MEMBERS ||
                if (request.create) members.isNotEmpty()
                else partyId != request.partyId || members.isEmpty()
        )
            return false
        partyId = request.partyId
        members.add(id)
        return true
    }

    fun leave(id: String) {
        members.remove(id)
        ready.remove(id)
    }

    fun view() = PartyView(partyId, members.toList(), members.map { it in ready })

    fun entryRejection(id: String): String? =
        when {
            members.firstOrNull() != id -> Rules.NOT_LEADER
            members.any { it !in ready } -> Rules.NOT_READY
            else -> null
        }
}
