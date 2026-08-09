package systems.zlink.samples.zoneworld.server.ops;

import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.List;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeRegistry;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class OpsSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final NodeRegistry registry;
    private final MaintenanceStore maintenance;
    private final ZLinkFanoutClient fanout;

    public OpsSession(
        ZLinkSessionContext context,
        NodeRegistry registry,
        MaintenanceStore maintenance,
        ZLinkFanoutClient fanout) {
        this.context = context;
        this.registry = registry;
        this.maintenance = maintenance;
        this.fanout = fanout;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return switch (dispatch.packetName()) {
            case "WatchNodesReq" -> reply(new Messages.WatchNodesRes(registry.snapshot()));
            case "AnnounceWorldReq" -> announce(payload.decode(Messages.AnnounceWorldReq.class));
            case "SetMaintenanceReq" -> setMaintenance(
                payload.decode(Messages.SetMaintenanceReq.class));
            case "NodeDiagnosticsReq" -> diagnostics(
                payload.decode(Messages.NodeDiagnosticsReq.class));
            default -> throw new IllegalStateException(
                "Unknown ZoneWorld Ops packet: " + dispatch.packetName());
        };
    }

    private CompletionStage<Void> announce(Messages.AnnounceWorldReq request) {
        String id = UUID.randomUUID().toString();
        return fanout.publish(
                ZoneWorldNames.BROADCAST_CHANNEL,
                ZoneWorldNames.ANNOUNCE_TOPIC,
                new Messages.WorldAnnounceEvent(id, request.text()))
            .submit()
            .thenCompose(ignored -> reply(new Messages.AnnounceWorldRes(id)));
    }

    private CompletionStage<Void> setMaintenance(Messages.SetMaintenanceReq request) {
        if (ZoneWorldSpec.zonesOf(request.nodeId()).isEmpty()) {
            return reply(new Messages.SetMaintenanceRes(
                request.nodeId(), false, List.of(), "UnknownNode"));
        }
        maintenance.set(request.nodeId(), request.enabled());
        registry.setMaintenance(request.nodeId(), request.enabled());
        return fanout.publish(
                ZoneWorldNames.BROADCAST_CHANNEL,
                ZoneWorldNames.MAINTENANCE_TOPIC,
                new Messages.NodeMaintenanceChangedEvent(request.nodeId(), request.enabled()))
            .submit()
            .thenCompose(ignored -> reply(new Messages.SetMaintenanceRes(
                request.nodeId(),
                request.enabled(),
                ZoneWorldSpec.zonesOf(request.nodeId()),
                null)));
    }

    private CompletionStage<Void> diagnostics(Messages.NodeDiagnosticsReq request) {
        Messages.NodeView node = registry.find(request.nodeId());
        if (node == null) {
            return reply(new Messages.NodeDiagnosticsRes(
                request.nodeId(), ZoneWorldSpec.zonesOf(request.nodeId()),
                0, maintenance.get(request.nodeId()), "UnknownNode"));
        }
        return reply(new Messages.NodeDiagnosticsRes(
            node.nodeId(), node.zones(), node.playerCount(), node.maintenance(), null));
    }

    private CompletionStage<Void> reply(Object message) {
        return context.client().reply(message).submit();
    }
}
