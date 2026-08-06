package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
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
        return routes.sendToSpot(targetSpotRid.toString(), command)
            .submit().thenApply(ignored -> null);
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

    public static final class CounterReset
        extends SpotCommandHandler<Contracts.CounterResetMsg> {
        public CounterReset(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.CounterResetMsg.class);
        }
    }

    public static final class CounterAwait
        extends SpotCommandHandler<Contracts.CounterAwaitMsg> {
        public CounterAwait(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.CounterAwaitMsg.class);
        }
    }

    public static final class IoWorker
        extends SpotCommandHandler<Contracts.IoWorkerMsg> {
        public IoWorker(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.IoWorkerMsg.class);
        }
    }

    public static final class CpuWorker
        extends SpotCommandHandler<Contracts.CpuWorkerMsg> {
        public CpuWorker(ZLinkRouteClient routes, SpotHandleResolver spots) {
            super(routes, spots, Contracts.CpuWorkerMsg.class);
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
            return routes.requestToSpot(
                        targetSpotRid.toString(),
                        request)
                    .submit(Contracts.ProbeRes.class)
                .thenAccept(reply -> context.client().reply(reply).submit());
        }
    }

    public static final class CounterReadRequest
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.CounterReadReq> {
        private final ZLinkRouteClient routes;

        public CounterReadRequest(ZLinkRouteClient routes) {
            this.routes = routes;
        }

        @Override
        public Class<Contracts.CounterReadReq> messageType() {
            return Contracts.CounterReadReq.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.CounterReadReq request) {
            String spotId = dispatch.metadata()
                .getOrDefault(Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT);
            return routes.requestToSpot(spotId, request)
                .submit(Contracts.CounterReadRes.class)
                .thenAccept(reply -> context.client().reply(reply).submit());
        }
    }

    public static final class IoWorkerBatchRequest
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.IoWorkerBatchReq> {
        private final ZLinkRouteClient routes;

        public IoWorkerBatchRequest(ZLinkRouteClient routes) {
            this.routes = routes;
        }

        @Override
        public Class<Contracts.IoWorkerBatchReq> messageType() {
            return Contracts.IoWorkerBatchReq.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.IoWorkerBatchReq request) {
            String spotId = dispatch.metadata()
                .getOrDefault(Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT);
            return routes.requestToSpot(spotId, request)
                .timeout(Duration.ofSeconds(10))
                .submit(Contracts.IoWorkerBatchRes.class)
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
