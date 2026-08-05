package systems.zlink.e2e.kotlin.spotservice.play.spots

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext

class ScenarioActor(
    private val actorId: String,
    private val context: ZLinkActorContext
) : ZLinkActor {
    private var profile = Contracts.ActorProfile("", 0, listOf())
    private var sequence = 0

    fun actorId(): String = actorId

    override fun context(): ZLinkActorContext = context

    fun nextSequence(): Int {
        sequence += 1
        return sequence
    }

    fun applyProfile(next: Contracts.ActorProfile) {
        profile = next
    }

    fun profile(): Contracts.ActorProfile = profile
}
