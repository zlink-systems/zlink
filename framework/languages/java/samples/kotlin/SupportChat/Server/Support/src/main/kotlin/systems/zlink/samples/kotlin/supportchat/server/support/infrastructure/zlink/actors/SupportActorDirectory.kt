package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

import systems.zlink.framework.actors.ActorRef

data class SupportActorDirectoryEntry(
    val actor: SupportUserActor,
    val ref: ActorRef,
    val displayName: String,
    val role: String,
)

class SupportActorDirectory {
    private val actors = linkedMapOf<String, SupportActorDirectoryEntry>()

    fun addOrUpdate(actor: SupportUserActor, actorRef: ActorRef) {
        actors[actor.actorId] = SupportActorDirectoryEntry(
            actor = actor,
            ref = actorRef,
            displayName = actor.displayName,
            role = actor.role,
        )
    }

    fun get(actorId: String): SupportActorDirectoryEntry =
        actors[actorId] ?: throw IllegalStateException("Support actor is not available. actor=$actorId")
}
