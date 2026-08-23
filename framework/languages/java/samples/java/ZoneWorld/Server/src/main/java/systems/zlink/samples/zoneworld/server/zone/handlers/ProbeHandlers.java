package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.HexFormat;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;

@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class ProbeHandlers {
    @ZLinkSpotActorRequest
    public CompletionStage<Messages.MessageFollowProbeRes> request(
        ZoneSpot spot, PlayerActor actor, ZLinkMessageContext context,
        Messages.MessageFollowProbeReq request) {
        System.out.println("message-follow probe handled. actor=" + actor.actorId()
            + ", probe=" + request.probeId() + ", payload="
            + HexFormat.of().withUpperCase().formatHex(request.payload()));
        return CompletableFuture.completedFuture(
            new Messages.MessageFollowProbeRes(request.probeId(), request.payload()));
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> send(
        ZoneSpot spot, PlayerActor actor, ZLinkMessageContext context,
        Messages.MessageFollowProbeMsg message) {
        System.out.println("message-follow probe one-way handled. actor=" + actor.actorId()
            + ", probe=" + message.probeId() + ", payload="
            + HexFormat.of().withUpperCase().formatHex(message.payload()));
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> crash(
        ZoneSpot spot, PlayerActor actor, ZLinkMessageContext context,
        Messages.CrashRelocationProbeMsg message) {
        return spot.crashProbe(actor, message.x(), message.y());
    }
}
