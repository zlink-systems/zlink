package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode

class ActorDirectory {
    private val actors = mutableMapOf<String, CourierActor>()

    fun register(actor: CourierActor) {
        synchronized(actors) {
            actors[actor.actorId()] = actor
        }
    }

    fun require(actorId: String): CourierActor =
        synchronized(actors) { actors[actorId] }
            ?: throw IllegalStateException("Courier actor is not registered: $actorId")

    fun remove(actorId: String) {
        synchronized(actors) {
            actors.remove(actorId)
        }
    }
}
