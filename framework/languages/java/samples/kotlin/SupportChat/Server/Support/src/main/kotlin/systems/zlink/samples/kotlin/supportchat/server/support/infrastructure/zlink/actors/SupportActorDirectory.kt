package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

class SupportActorDirectory {
    private val actors = linkedMapOf<String, SupportUserActor>()

    fun addOrUpdate(actor: SupportUserActor) {
        actors[actor.actorId] = actor
    }

    fun get(actorId: String): SupportUserActor =
        actors[actorId]
            ?: throw IllegalStateException("Support actor is not available. actor=$actorId")
}
