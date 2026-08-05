package systems.zlink.e2e.automaticturn.shared;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public abstract class SpotCommandHandler<TCommand>
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, TCommand> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;
    private final Class<TCommand> messageType;

    protected SpotCommandHandler(
        ZLinkRouteClient routes,
        SpotHandleResolver spots,
        Class<TCommand> messageType) {
        this.routes = routes;
        this.spots = spots;
        this.messageType = messageType;
    }

    @Override
    public Class<TCommand> messageType() {
        return messageType;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        TCommand command) {
        RoutingId targetSpotRid = RoutingId.from(dispatch.metadata()
            .getOrDefault(Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT));
        return spots.resolveSpotHandle(targetSpotRid).thenCompose(handle -> routes.sendToSpot(
            handle.orElseThrow(() -> new IllegalStateException("spot not found: " + targetSpotRid)),
            command).submit().thenApply(ignored -> null));
    }

    public static final class WorkerAwait
        extends SpotCommandHandler<Contracts.WorkerAwaitMsg> {
        public WorkerAwait(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.WorkerAwaitMsg.class);
        }
    }

    public static final class Await
        extends SpotCommandHandler<Contracts.AwaitMsg> {
        public Await(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.AwaitMsg.class);
        }
    }

    public static final class Probe
        extends SpotCommandHandler<Contracts.ProbeMsg> {
        public Probe(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.ProbeMsg.class);
        }
    }

    public static final class ProbeRequest
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ProbeReq> {
        private final ZLinkRouteClient routes;
        private final SpotHandleResolver spots;

        public ProbeRequest(ZLinkRouteClient routes, SpotHandleResolver spots) {
            this.routes = routes;
            this.spots = spots;
        }

        @Override
        public Class<Contracts.ProbeReq> messageType() {
            return Contracts.ProbeReq.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.ProbeReq request) {
            RoutingId targetSpotRid = RoutingId.from(dispatch.metadata()
                .getOrDefault(Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT));
            return spots.resolveSpotHandle(targetSpotRid)
                .thenCompose(handle -> routes.requestToSpot(
                        handle.orElseThrow(() ->
                            new IllegalStateException("spot not found: " + targetSpotRid)),
                        request)
                    .submit(Contracts.ProbeRes.class))
                .thenAccept(reply -> context.client().reply(reply).submit());
        }
    }

    public static final class AwaitTimeout
        extends SpotCommandHandler<Contracts.AwaitTimeoutMsg> {
        public AwaitTimeout(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.AwaitTimeoutMsg.class);
        }
    }

    public static final class AwaitCancel
        extends SpotCommandHandler<Contracts.AwaitCancelMsg> {
        public AwaitCancel(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.AwaitCancelMsg.class);
        }
    }

    public static final class TimerStart
        extends SpotCommandHandler<Contracts.TimerStartMsg> {
        public TimerStart(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.TimerStartMsg.class);
        }
    }

    public static final class TimerStop
        extends SpotCommandHandler<Contracts.TimerStopMsg> {
        public TimerStop(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.TimerStopMsg.class);
        }
    }
}
