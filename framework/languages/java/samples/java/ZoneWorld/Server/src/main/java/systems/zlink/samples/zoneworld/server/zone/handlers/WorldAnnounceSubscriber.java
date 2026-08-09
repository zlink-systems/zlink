package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
public final class WorldAnnounceSubscriber
    implements ZLinkFanoutHandler<Messages.WorldAnnounceEvent> {
    private final ZLinkRouteClient routes;
    private final SampleTopology topology;

    public WorldAnnounceSubscriber(ZLinkRouteClient routes, SampleTopology topology) {
        this.routes = routes;
        this.topology = topology;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.WorldAnnounceEvent message,
        ZLinkPublishMessageContext context) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (String zone : ZoneWorldSpec.zonesOf(topology.nodeId())) {
            sends = sends.thenCompose(ignored -> routes.sendToSpot(
                    zone,
                    new Messages.DeliverAnnounceMsg(
                        message.announcementId(), message.text()))
                .submit());
        }
        return sends.thenRun(() -> System.out.println(
            "fanout announcement delivered node=" + topology.nodeId()
                + " id=" + message.announcementId()));
    }
}
