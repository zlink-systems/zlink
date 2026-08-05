package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.ObserveMilestoneReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.ObserveMilestoneRes

@ZLinkHandlerGroup(SampleNames.PlayActor)
class PlayActorObserveMilestoneHandler {
    @ZLinkSpotActorRequest
    suspend fun observe(
        entrySpot: PlayEntrySpot,
        actor: PlayActor,
        context: ZLinkMessageContext,
        request: ObserveMilestoneReq,
    ): ObserveMilestoneRes = entrySpot.observeMilestone(actor)
}
