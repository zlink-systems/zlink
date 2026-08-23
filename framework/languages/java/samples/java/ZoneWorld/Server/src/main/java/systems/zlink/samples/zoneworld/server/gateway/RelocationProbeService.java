package systems.zlink.samples.zoneworld.server.gateway;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.stereotype.Component;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;

@Component
@ConditionalOnProperty(prefix = "sample", name = "role", havingValue = "gateway")
public final class RelocationProbeService {
    private static final List<List<String>> ADJACENT = List.of(
        List.of("zone-nw", "zone-ne"), List.of("zone-nw", "zone-sw"),
        List.of("zone-ne", "zone-se"), List.of("zone-sw", "zone-se"));
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;

    public RelocationProbeService(ZLinkSpotManager spots, ZLinkActorManager actors) {
        this.spots = spots;
        this.actors = actors;
    }

    public CompletionStage<Messages.RelocationPairRes> selectPair() {
        return selectPair(0);
    }

    private CompletionStage<Messages.RelocationPairRes> selectPair(int index) {
        if (index >= ADJACENT.size()) return java.util.concurrent.CompletableFuture.completedFuture(
            new Messages.RelocationPairRes("", "", "", "", "NoCrossNodeAdjacentPair"));
        List<String> pair = ADJACENT.get(index);
        return spots.find(pair.get(0)).thenCombine(spots.find(pair.get(1)), (source, target) -> {
            if (source.isPresent() && target.isPresent()
                && !source.get().nodeRid().equals(target.get().nodeRid())) {
                return new Messages.RelocationPairRes(pair.get(0), pair.get(1),
                    source.get().nodeRid().toString(), target.get().nodeRid().toString(), null);
            }
            return null;
        }).thenCompose(found -> found != null
            ? java.util.concurrent.CompletableFuture.completedFuture(found)
            : selectPair(index + 1));
    }

    public CompletionStage<Messages.ActorLocationProbeRes> findActor(String actorId) {
        return actors.find(actorId).thenApply(found -> found
            .map(actor -> new Messages.ActorLocationProbeRes(actor.actorId(),
                actor.objectGeneration(), actor.nodeRid().toString(), null))
            .orElseGet(() -> new Messages.ActorLocationProbeRes(
                actorId, 0, "", "ActorNotFound")));
    }

    public CompletionStage<Messages.FreshActorProbeRes> createFresh(String actorId) {
        return actors.getOrCreate(actorId, ZoneWorldNames.PLAYER_ACTOR_TYPE)
            .inMesh(ZoneWorldNames.MESH).request(ZLinkMessage.empty()).submit()
            .thenApply(result -> {
                ActorRef actor = result instanceof ZLinkActorCreateResult.Created created
                    ? created.actor()
                    : result instanceof ZLinkActorCreateResult.Existing existing
                        ? existing.actor() : null;
                return actor == null
                    ? new Messages.FreshActorProbeRes(actorId, 0, "", "ActorCreateRejected")
                    : new Messages.FreshActorProbeRes(actor.actorId(), actor.objectGeneration(),
                        actor.nodeRid().toString(), null);
            });
    }
}
