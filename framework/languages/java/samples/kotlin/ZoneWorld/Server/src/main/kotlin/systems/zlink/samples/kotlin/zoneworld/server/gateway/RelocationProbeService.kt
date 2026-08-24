package systems.zlink.samples.kotlin.zoneworld.server.gateway

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty
import org.springframework.stereotype.Component
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames

@Component
@ConditionalOnProperty(prefix = "sample", name = ["role"], havingValue = "gateway")
class RelocationProbeService(
    private val spots: ZLinkSpotManager,
    private val actors: ZLinkActorManager,
) {
    private val adjacent = listOf(
        listOf("zone-nw", "zone-ne"), listOf("zone-nw", "zone-sw"),
        listOf("zone-ne", "zone-se"), listOf("zone-sw", "zone-se"),
    )

    fun selectPair(index: Int = 0): CompletionStage<Messages.RelocationPairRes> {
        if (index >= adjacent.size) return CompletableFuture.completedFuture(
            Messages.RelocationPairRes("", "", "", "", "NoCrossNodeAdjacentPair"),
        )
        val pair = adjacent[index]
        return spots.find(pair[0]).thenCombine(spots.find(pair[1])) { source, target ->
            if (source.isPresent && target.isPresent && source.get().nodeRid() != target.get().nodeRid()) {
                Messages.RelocationPairRes(pair[0], pair[1], source.get().nodeRid().toString(),
                    target.get().nodeRid().toString())
            } else null
        }.thenCompose { found ->
            if (found != null) CompletableFuture.completedFuture(found) else selectPair(index + 1)
        }
    }

    fun findActor(actorId: String): CompletionStage<Messages.ActorLocationProbeRes> =
        actors.find(actorId).thenApply { found ->
            if (found.isPresent) found.get().let {
                Messages.ActorLocationProbeRes(it.actorId(), it.objectGeneration(), it.nodeRid().toString())
            } else Messages.ActorLocationProbeRes(actorId, 0, "", "ActorNotFound")
        }

    fun createFresh(actorId: String): CompletionStage<Messages.FreshActorProbeRes> =
        actors.getOrCreate(actorId, ZoneWorldNames.PLAYER_ACTOR_TYPE)
            .inMesh(ZoneWorldNames.MESH).request(ZLinkMessage.empty()).submit()
            .thenApply { result ->
                val actor = when (result) {
                    is ZLinkActorCreateResult.Created -> result.actor()
                    is ZLinkActorCreateResult.Existing -> result.actor()
                    is ZLinkActorCreateResult.Rejected -> null
                }
                actor?.let { Messages.FreshActorProbeRes(
                    it.actorId(), it.objectGeneration(), it.nodeRid().toString(),
                ) } ?: Messages.FreshActorProbeRes(actorId, 0, "", "ActorCreateRejected")
            }
}
