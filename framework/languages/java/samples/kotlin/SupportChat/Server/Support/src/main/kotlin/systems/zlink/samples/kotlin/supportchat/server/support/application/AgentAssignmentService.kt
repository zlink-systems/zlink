package systems.zlink.samples.kotlin.supportchat.server.support.application

class AgentAssignmentService(
    private val availability: AgentAvailabilityDirectory,
) {
    private val reservations = linkedMapOf<String, String>()

    fun setAvailable(
        rosterActorId: String,
        displayName: String,
        isAvailable: Boolean,
    ) {
        val activeConversations = reservations.values.count { it == rosterActorId }
        availability.setAvailable(rosterActorId, displayName, isAvailable, activeConversations)
    }

    fun assignForConversation(conversationId: String): AvailableAgent? {
        if (reservations.containsKey(conversationId)) {
            return null
        }
        val assigned = availability.assign() ?: return null
        reservations[conversationId] = assigned.rosterActorId
        return assigned
    }

    fun releaseConversation(conversationId: String) {
        val rosterActorId = reservations.remove(conversationId) ?: return
        availability.release(rosterActorId)
    }
}
