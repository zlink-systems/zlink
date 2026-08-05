package systems.zlink.samples.kotlin.supportchat.server.support.application

class AgentAvailabilityDirectory(
    private val capacity: Int,
) {
    private val agents = linkedMapOf<String, AgentSlot>()

    fun setAvailable(
        rosterActorId: String,
        displayName: String,
        isAvailable: Boolean,
        activeConversations: Int,
    ) {
        if (!isAvailable) {
            agents.remove(rosterActorId)
            return
        }

        val slot = agents.getOrPut(rosterActorId) { AgentSlot(rosterActorId) }
        slot.displayName = displayName
        slot.active = activeConversations
    }

    fun assign(): AvailableAgent? {
        val picked = agents.values.firstOrNull { it.active < capacity } ?: return null
        picked.active += 1
        agents.remove(picked.rosterActorId)
        agents[picked.rosterActorId] = picked
        return AvailableAgent(picked.rosterActorId, picked.displayName)
    }

    fun release(rosterActorId: String) {
        val slot = agents[rosterActorId] ?: return
        if (slot.active > 0) {
            slot.active -= 1
        }
    }

    private data class AgentSlot(
        val rosterActorId: String,
        var displayName: String = "",
        var active: Int = 0,
    )
}

data class AvailableAgent(
    val rosterActorId: String,
    val displayName: String,
)
