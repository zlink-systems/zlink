package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;

public final class AwaitProbeHandlers {
    public static final class PersistentRoomStateHandler
        implements systems.zlink.framework.spots.ZLinkSpotRequestHandler<AwaitProbeSpot,
            Contracts.PersistentRoomStateReq, Contracts.PersistentRoomStateRes> {
        @Override
        public java.util.concurrent.CompletionStage<Contracts.PersistentRoomStateRes> handle(
            AwaitProbeSpot spot,
            Contracts.PersistentRoomStateReq request) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                spot.persistentState(request));
        }
    }
    private AwaitProbeHandlers() {
    }

    private static Duration delayRequestTimeout(long delayMillis) {
        return Duration.ofMillis(delayMillis).plusSeconds(5);
    }

    public static final class HoldHandler {
        private final EvidenceStore evidence;

        public HoldHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @ZLinkSpotRequest
        public CompletionStage<Contracts.ScenarioRes> handle(
            AwaitProbeSpot spot,
            Contracts.HoldReq request) {
            String value = spot.context().spotId();
            evidence.record("hold-started", request.requestId(), value);
            return spot.context().outbound()
                .requestToChannel(Contracts.DELAY_CHANNEL, new Contracts.DelayReq(request.requestId(), 800))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
                .thenApply(reply -> {
                    evidence.record("hold-resumed", request.requestId(), value);
                    evidence.record("hold-completed", request.requestId(), value);
                    return new Contracts.ScenarioRes("ATD-A1", request.requestId(), "ok");
                });
        }
    }

    public static final class ObservabilityQueueHandler {
        @ZLinkSpotRequest
        public CompletionStage<Contracts.ScenarioRes> handle(
            AwaitProbeSpot spot,
            Contracts.ObservabilityQueueReq request) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return CompletableFuture.failedFuture(error);
            }
            return CompletableFuture.completedFuture(
                new Contracts.ScenarioRes("OBS-B2-QUEUE", request.requestId(), "ok"));
        }
    }

    public static final class AwaitHandler {
        private final EvidenceStore evidence;

        public AwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @ZLinkSpotRequest
        public CompletionStage<Contracts.ScenarioRes> handle(
            AwaitProbeSpot spot,
            Contracts.AwaitReq request) {
            String value = "spot=" + spot.context().spotId() + ";correlation=" + request.correlationId();
            long delayMillis = "ATD-E3".equals(request.scenarioId()) ? 30_000
                : request.scenarioId().startsWith("TD-") ? 2_000 : 800;
            boolean yields = "TD-B1".equals(request.scenarioId());
            boolean turnContract = request.scenarioId().startsWith("TD-");
            String waitingMarker = yields ? "yield-released"
                : turnContract ? "await-held" : "await-released";
            String resumedMarker = yields ? "yield-resumed" : "await-resumed";
            evidence.record("await-started", request.requestId(), value);
            evidence.record(waitingMarker, request.requestId(), value);
            var call = spot.context().outbound()
                .requestToChannel(Contracts.DELAY_CHANNEL, new Contracts.DelayReq(request.requestId(), delayMillis))
                .timeout(delayRequestTimeout(delayMillis));
            CompletionStage<Contracts.DelayRes> completion = yields
                ? call.yield(Contracts.DelayRes.class)
                : call.submit(Contracts.DelayRes.class);
            return completion
                .thenApply(reply -> {
                    evidence.record(resumedMarker, request.requestId(), value);
                    evidence.record(turnContract ? "completed" : "await-completed",
                        request.requestId(), value);
                    return new Contracts.ScenarioRes(request.scenarioId(), request.requestId(), evidence.nodeRid());
                });
        }
    }

    public static final class WorkerAwaitHandler {
        private final EvidenceStore evidence;

        public WorkerAwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @ZLinkSpotRequest
        public CompletionStage<Contracts.ScenarioRes> handle(
            AwaitProbeSpot spot,
            Contracts.WorkerAwaitReq request) {
            String value = spot.context().spotId();
            evidence.record("worker-await-started", request.requestId(), value);
            evidence.record("worker-await-released", request.requestId(), value);
            return spot.context().runCpuWorker(cancellation -> {
                    Thread.sleep(2000);
                    return request.requestId();
                })
                .timeout(Duration.ofSeconds(10))
                .submit()
                .thenApply(result -> {
                    evidence.record("worker-await-resumed", result, value);
                    evidence.record("worker-await-completed", result, value);
                    return new Contracts.ScenarioRes("ATD-A4", request.requestId(), "ok");
                });
        }
    }

    public static final class ProbeHandler {
        private final EvidenceStore evidence;

        public ProbeHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @ZLinkSpotRequest
        public CompletionStage<Contracts.ProbeRes> handle(
            AwaitProbeSpot spot,
            Contracts.ProbeReq request) {
            recordProbe(evidence, spot, request.requestId());
            return CompletableFuture.completedFuture(new Contracts.ProbeRes(request.requestId()));
        }
    }

    public static final class WorkerAwaitMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.WorkerAwaitMsg> {
        private final EvidenceStore evidence;

        public WorkerAwaitMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.WorkerAwaitMsg request) {
            String value = spot.context().spotId();
            evidence.record("worker-await-started", request.requestId(), value);
            evidence.record("worker-await-released", request.requestId(), value);
            return spot.context().runCpuWorker(cancellation -> {
                    Thread.sleep(request.delayMillis());
                    return request.requestId();
                })
                .timeout(Duration.ofSeconds(10))
                .submit()
                .thenAccept(result -> {
                    evidence.record("worker-await-resumed", result, value);
                    evidence.record("worker-await-completed", result, value);
                });
        }
    }

    public static final class CounterResetMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.CounterResetMsg> {
        private final EvidenceStore evidence;

        public CounterResetMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.CounterResetMsg request) {
            spot.resetCounter(request.value());
            evidence.record("counter-reset", request.requestId(), "value=" + request.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class CounterAwaitMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.CounterAwaitMsg> {
        private final EvidenceStore evidence;

        public CounterAwaitMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.CounterAwaitMsg request) {
            int before = spot.counter();
            evidence.record("counter-before", request.requestId(),
                request.operationId() + ";value=" + before);
            var call = spot.context().outbound()
                .requestToChannel(Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), request.delayMillis()))
                .timeout(delayRequestTimeout(request.delayMillis()));
            CompletionStage<Contracts.DelayRes> completion = "yield".equals(request.terminator())
                ? call.yield(Contracts.DelayRes.class)
                : call.submit(Contracts.DelayRes.class);
            return completion.thenAccept(ignored -> {
                int observed = spot.counter();
                int value = spot.incrementCounter();
                evidence.record("counter-after-yield", request.requestId(),
                    request.operationId() + ";before=" + before + ";observed=" + observed
                        + ";value=" + value);
                evidence.record("counter-operation-completed", request.requestId(),
                    request.operationId());
            });
        }
    }

    public static final class CounterReadHandler
        implements systems.zlink.framework.spots.ZLinkSpotRequestHandler<AwaitProbeSpot,
            Contracts.CounterReadReq, Contracts.CounterReadRes> {
        @Override
        public CompletionStage<Contracts.CounterReadRes> handle(
            AwaitProbeSpot spot, Contracts.CounterReadReq request) {
            return CompletableFuture.completedFuture(
                new Contracts.CounterReadRes(request.requestId(), spot.counter()));
        }
    }

    public static final class IoWorkerMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.IoWorkerMsg> {
        private final EvidenceStore evidence;

        public IoWorkerMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.IoWorkerMsg request) {
            evidence.record("io-worker-started", request.requestId(), request.terminator());
            var call = spot.context().runIoWorker(cancellation -> {
                return CompletableFuture.supplyAsync(
                    () -> request.requestId(),
                    CompletableFuture.delayedExecutor(
                        request.delayMillis(), TimeUnit.MILLISECONDS));
            }).timeout(Duration.ofSeconds(10));
            CompletionStage<String> result = "yield".equals(request.terminator())
                ? call.yield() : call.submit();
            return result.thenAccept(value -> {
                evidence.record("io-worker-resumed", request.requestId(), value);
                evidence.record("io-worker-completed", request.requestId(), request.terminator());
            });
        }
    }

    public static final class IoWorkerBatchReqHandler
        implements systems.zlink.framework.spots.ZLinkSpotRequestHandler<AwaitProbeSpot,
            Contracts.IoWorkerBatchReq, Contracts.IoWorkerBatchRes> {
        private final EvidenceStore evidence;

        public IoWorkerBatchReqHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.IoWorkerBatchRes> handle(
            AwaitProbeSpot spot, Contracts.IoWorkerBatchReq request) {
            java.util.List<CompletionStage<String>> calls = new java.util.ArrayList<>();
            for (int index = 0; index < request.count(); index++) {
                int operation = index;
                calls.add(spot.context().runIoWorker(cancellation -> {
                    return CompletableFuture.supplyAsync(
                        () -> request.requestId() + "-" + operation,
                        CompletableFuture.delayedExecutor(
                            request.delayMillis(), TimeUnit.MILLISECONDS));
                }).yield());
            }
            return CompletableFuture.allOf(calls.stream()
                    .map(stage -> stage.toCompletableFuture())
                    .toArray(CompletableFuture[]::new))
                .thenApply(ignored -> {
                    evidence.record("io-worker-batch-completed", request.requestId(),
                        "count=" + request.count());
                    return new Contracts.IoWorkerBatchRes(request.requestId(), request.count());
                });
        }
    }

    public static final class CpuWorkerMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.CpuWorkerMsg> {
        private final EvidenceStore evidence;

        public CpuWorkerMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.CpuWorkerMsg request) {
            evidence.record("cpu-worker-started", request.requestId(), request.terminator());
            var call = spot.context().runCpuWorker(cancellation -> {
                Thread.sleep(request.delayMillis());
                return request.requestId();
            }).timeout(Duration.ofSeconds(10));
            CompletionStage<String> result = "yield".equals(request.terminator())
                ? call.yield() : call.submit();
            return result.thenAccept(value -> {
                evidence.record("cpu-worker-resumed", request.requestId(), value);
                evidence.record("cpu-worker-completed", request.requestId(), request.terminator());
            });
        }
    }

    public static final class ProbeMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.ProbeMsg> {
        private final EvidenceStore evidence;

        public ProbeMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.ProbeMsg request) {
            recordProbe(evidence, spot, request.requestId());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class AwaitMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.AwaitMsg> {
        private final EvidenceStore evidence;

        public AwaitMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.AwaitMsg request) {
            String value = "spot=" + spot.context().spotId()
                + ";correlation=" + request.correlationId() + ";handler=spot";
            boolean yields = request.correlationId().startsWith("TD-B");
            boolean turnContract = request.correlationId().startsWith("TD-");
            evidence.record("await-started", request.requestId(), value);
            evidence.record(yields ? "yield-released"
                    : turnContract ? "await-held" : "await-released",
                request.requestId(), value);
            var call = spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), request.delayMillis()))
                .timeout(delayRequestTimeout(request.delayMillis()));
            CompletionStage<Contracts.DelayRes> completion = yields
                ? call.yield(Contracts.DelayRes.class)
                : call.submit(Contracts.DelayRes.class);
            return completion
                .thenAccept(reply -> {
                    evidence.record(yields ? "yield-resumed" : "await-resumed",
                        request.requestId(), value);
                    evidence.record(turnContract ? "completed" : "await-completed",
                        request.requestId(), value);
                });
        }
    }

    public static final class AwaitTimeoutMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.AwaitTimeoutMsg> {
        private final EvidenceStore evidence;

        public AwaitTimeoutMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.AwaitTimeoutMsg request) {
            String value = "spot=" + spot.context().spotId() + ";handler=spot";
            evidence.record("timeout-await-started", request.requestId(), value);
            evidence.record("timeout-await-released", request.requestId(), value);
            return spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), request.delayMillis()))
                .timeout(Duration.ofMillis(request.timeoutMillis()))
                .submit(Contracts.DelayRes.class)
                .handle((reply, error) -> {
                    if (error == null) {
                        evidence.record("timeout-await-unexpected-resumed", request.requestId(), value);
                    } else {
                        evidence.record("timeout-await-completed", request.requestId(), value + ";error=" + error);
                    }
                    return null;
                });
        }
    }

    public static final class AwaitCancelMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.AwaitCancelMsg> {
        private final EvidenceStore evidence;

        public AwaitCancelMsgHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.AwaitCancelMsg request) {
            String value = "spot=" + spot.context().spotId() + ";handler=spot";
            evidence.record("cancel-await-started", request.requestId(), value);
            evidence.record("cancel-await-released", request.requestId(), value);
            CompletableFuture<Contracts.DelayRes> pending = spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), request.delayMillis()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
                .toCompletableFuture();
            CompletableFuture.delayedExecutor(request.cancelAfterMillis(), TimeUnit.MILLISECONDS)
                .execute(() -> pending.cancel(true));
            return pending.handle((reply, error) -> {
                if (error == null) {
                    evidence.record("cancel-await-unexpected-resumed", request.requestId(), value);
                } else {
                    evidence.record("cancel-await-completed", request.requestId(), value + ";error=" + error);
                }
                return null;
            });
        }
    }

    public static final class RemoteSpotAwaitHandler {
        private final EvidenceStore evidence;
        private final SpotHandleResolver spots;

        public RemoteSpotAwaitHandler(EvidenceStore evidence, SpotHandleResolver spots) {
            this.evidence = evidence;
            this.spots = spots;
        }

        @ZLinkSpotRequest
        public CompletionStage<Contracts.ScenarioRes> handle(
            AwaitProbeSpot spot,
            Contracts.RemoteSpotAwaitReq request) {
            String value = "spot=" + spot.context().spotId() + ";target=" + request.targetSpotRid();
            evidence.record("remote-await-started", request.requestId(), value);
            evidence.record("remote-await-released", request.requestId(), value);
            return spot.context().outbound()
                    .requestToSpot(request.targetSpotRid(),
                        new Contracts.AwaitReq("ATD-D2", request.requestId(), "remote-spot"))
                    .timeout(Duration.ofSeconds(5))
                    .submit(Contracts.ScenarioRes.class)
                .thenApply(targetReply -> {
                    String resumed = value + ";targetNode=" + targetReply.result();
                    evidence.record("remote-await-resumed", request.requestId(), resumed);
                    evidence.record("remote-await-completed", request.requestId(), resumed);
                    return new Contracts.ScenarioRes("ATD-D2", request.requestId(), evidence.nodeRid());
                });
        }
    }

    public static final class TimerStartMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.TimerStartMsg> {
        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.TimerStartMsg request) {
            return spot.startTimer(request);
        }
    }

    public static final class TimerStopMsgHandler
        implements ZLinkSpotPacketHandler<AwaitProbeSpot, Contracts.TimerStopMsg> {
        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, Contracts.TimerStopMsg request) {
            spot.stopTimers(request.requestId());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TimerTickHandler implements ZLinkSpotTimerHandler<AwaitProbeSpot> {
        private final EvidenceStore evidence;

        public TimerTickHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(AwaitProbeSpot spot, ZLinkTimerTick tick) {
            AwaitProbeSpot.TimerScenario scenario = spot.timerScenario(tick.name());
            if (scenario == null) {
                return CompletableFuture.completedFuture(null);
            }
            String value = "timer=" + tick.name() + ";mailbox=timer:" + tick.name()
                + ";tick=" + tick.deliveryIndex() + ";spot=" + spot.context().spotId();
            boolean yield = "yield-on-first".equals(scenario.mode())
                || "yield-then-next".equals(scenario.mode());
            if (tick.deliveryIndex() == 1
                && ("await-on-first".equals(scenario.mode())
                    || "await-then-next".equals(scenario.mode())
                    || yield)) {
                evidence.record(yield ? "yield-released" : "timer-await-started",
                    scenario.requestId(), value);
                evidence.record(yield ? "yield-held" : "timer-await-released",
                    scenario.requestId(), value);
                var call = spot.context().outbound()
                    .requestToChannel(
                        Contracts.DELAY_CHANNEL,
                        new Contracts.DelayReq(scenario.requestId(), scenario.delayMillis()))
                    .timeout(delayRequestTimeout(scenario.delayMillis()));
                CompletionStage<Contracts.DelayRes> completion = yield
                    ? call.yield(Contracts.DelayRes.class)
                    : call.submit(Contracts.DelayRes.class);
                return completion
                    .thenAccept(reply -> {
                        evidence.record(yield ? "yield-resumed" : "timer-await-resumed",
                            scenario.requestId(), value);
                        evidence.record(yield ? "yield-completed" : "timer-await-completed",
                            scenario.requestId(), value);
                        if ("await-on-first".equals(scenario.mode())
                            || "yield-on-first".equals(scenario.mode())) {
                            spot.closeTimer(tick.name());
                        }
                    });
            }
            if (tick.deliveryIndex() == 2
                && ("await-then-next".equals(scenario.mode())
                    || "yield-then-next".equals(scenario.mode()))) {
                evidence.record("timer-next-started", scenario.requestId(), value);
                evidence.record("timer-next-completed", scenario.requestId(), value);
                spot.closeTimer(tick.name());
            } else if ("fast".equals(scenario.mode())) {
                evidence.record("timer-fast-started", scenario.requestId(), value);
                evidence.record("timer-fast-completed", scenario.requestId(), value);
                spot.closeTimer(tick.name());
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ObservabilityFanoutHandler
        implements ZLinkFanoutHandler<Contracts.ObservabilityFanoutEvent> {
        private final EvidenceStore evidence;

        public ObservabilityFanoutHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(
            Contracts.ObservabilityFanoutEvent event,
            ZLinkPublishMessageContext context) {
            evidence.record("obs-fanout-received", event.requestId(), context.topic());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ActorAwaitHandler
        implements ZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor,
            Contracts.ActorAwaitReq, Contracts.ActorAwaitRes> {
        private final EvidenceStore evidence;

        public ActorAwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorAwaitRes> handle(
            AwaitEntrySpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorAwaitReq request) {
            return actorAwait(spot.context().outbound(), spot.context().spotId(), actor, request, evidence);
        }
    }

    public static final class ActorJoinHandler
        implements ZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor,
            Contracts.ActorJoinReq, Contracts.ActorJoinRes> {
        private final EvidenceStore evidence;

        public ActorJoinHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorJoinRes> handle(
            AwaitEntrySpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorJoinReq request) {
            actor.context().joinSpot(request.spotRid(), "join")
                .timeout(Duration.ofSeconds(5))
                .defer();
            evidence.record("actor-joined", request.requestId(),
                "actor=" + actor.actorId() + ";spot=" + request.spotRid());
            return CompletableFuture.completedFuture(new Contracts.ActorJoinRes(
                "ATD-B-JOIN", request.requestId(), actor.actorId(), "joined"));
        }
    }

    public static final class ActorJoinAwaitHandler
        implements ZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor,
            Contracts.ActorJoinAwaitReq, Contracts.ActorJoinAwaitRes> {
        private final EvidenceStore evidence;

        public ActorJoinAwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorJoinAwaitRes> handle(
            AwaitEntrySpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorJoinAwaitReq request) {
            String value = actorValue(spot.context().spotId(), actor)
                + ";target=" + request.targetSpotRid();
            evidence.record("actor-join-await-started", request.requestId(), value);
            evidence.record("actor-join-await-released", request.requestId(), value);
            actor.context().joinSpot(
                    request.targetSpotRid(),
                    new Contracts.DelayReq(request.requestId(), 350))
                .timeout(Duration.ofSeconds(5))
                .defer();
            return spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), 350))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
                .thenApply(ignored -> {
                    String resumed = value + ";joined=" + request.targetSpotRid();
                    evidence.record("actor-join-await-resumed", request.requestId(), resumed);
                    evidence.record("actor-join-await-completed", request.requestId(), resumed);
                    return new Contracts.ActorJoinAwaitRes(
                        "ATD-B3", request.requestId(), actor.actorId(),
                        "actor-join-await-completed");
                });
        }
    }

    public static final class ActorPushNotifyAwaitHandler
        implements ZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor,
            Contracts.ActorPushAwaitReq, Contracts.ActorPushAwaitRes> {
        private final EvidenceStore evidence;

        public ActorPushNotifyAwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorPushAwaitRes> handle(
            AwaitEntrySpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorPushAwaitReq request) {
            String value = actorValue(spot.context().spotId(), actor) + ";handler=actor";
            evidence.record("actor-push-await-started", request.requestId(), value);
            evidence.record("actor-push-await-released", request.requestId(), value);
            return spot.context().outbound()
                .requestToChannel(
                    Contracts.DELAY_CHANNEL,
                    new Contracts.DelayReq(request.requestId(), request.delayMillis()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
                .thenApply(reply -> {
                    evidence.record("actor-push-await-resumed", request.requestId(), value);
                    actor.context().boundSession().send(new Contracts.ActorPushNotify(
                        actor.actorId(), request.requestId(), request.value(),
                        spot.context().nodeRid().toString())).submit();
                    evidence.record("actor-push-await-completed", request.requestId(), value);
                    return new Contracts.ActorPushAwaitRes(
                        "ATD-D4", request.requestId(), actor.actorId(), "actor-push-await-completed");
                });
        }
    }

    public static final class ActorFastHandler
        implements ZLinkEntrySpotActorRequestHandler<AwaitEntrySpot, AwaitActor,
            Contracts.ActorFastReq, Contracts.ActorFastRes> {
        private final EvidenceStore evidence;

        public ActorFastHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorFastRes> handle(
            AwaitEntrySpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorFastReq request) {
            return fast(spot.context().spotId(), actor, request, evidence);
        }
    }

    public static final class SpotActorAwaitHandler
        implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor,
            Contracts.ActorAwaitReq, Contracts.ActorAwaitRes> {
        private final EvidenceStore evidence;

        public SpotActorAwaitHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorAwaitRes> handle(
            AwaitProbeSpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorAwaitReq request) {
            return actorAwait(spot.context().outbound(), spot.context().spotId(), actor, request, evidence);
        }
    }

    public static final class SpotActorFastHandler
        implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor,
            Contracts.ActorFastReq, Contracts.ActorFastRes> {
        private final EvidenceStore evidence;

        public SpotActorFastHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorFastRes> handle(
            AwaitProbeSpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorFastReq request) {
            return fast(spot.context().spotId(), actor, request, evidence);
        }
    }

    public static final class SpotActorJoinHandler
        implements ZLinkSpotActorRequestHandler<AwaitProbeSpot, AwaitActor,
            Contracts.ActorJoinReq, Contracts.ActorJoinRes> {
        private final EvidenceStore evidence;

        public SpotActorJoinHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ActorJoinRes> handle(
            AwaitProbeSpot spot,
            AwaitActor actor,
            ZLinkMessageContext context,
            Contracts.ActorJoinReq request) {
            actor.context().joinSpot(request.spotRid(), "join")
                .timeout(Duration.ofSeconds(5))
                .defer();
            evidence.record("actor-joined", request.requestId(),
                "actor=" + actor.actorId() + ";spot=" + request.spotRid());
            return CompletableFuture.completedFuture(new Contracts.ActorJoinRes(
                "TD-E", request.requestId(), actor.actorId(), "joined"));
        }
    }

    private static CompletionStage<Contracts.ActorAwaitRes> actorAwait(
        systems.zlink.framework.spots.ZLinkSpotOutbound outbound,
        String spotRid,
        AwaitActor actor,
        Contracts.ActorAwaitReq request,
        EvidenceStore evidence) {
        String value = actorValue(spotRid, actor);
        evidence.record("actor-await-started", request.requestId(), value);
        evidence.record("actor-await-released", request.requestId(), value);
        var call = outbound.requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(request.requestId(), request.delayMillis()))
            .timeout(delayRequestTimeout(request.delayMillis()));
        CompletionStage<Contracts.DelayRes> completion = request.requestId().startsWith("atdb1-")
            ? call.yield(Contracts.DelayRes.class)
            : call.submit(Contracts.DelayRes.class);
        return completion
            .thenApply(reply -> {
                evidence.record("actor-await-resumed", request.requestId(), value);
                evidence.record("actor-await-completed", request.requestId(), value);
                return new Contracts.ActorAwaitRes(
                    "ATD-B", request.requestId(), actor.actorId(), "actor-await-completed");
            });
    }

    private static CompletionStage<Contracts.ActorFastRes> fast(
        String spotRid,
        AwaitActor actor,
        Contracts.ActorFastReq request,
        EvidenceStore evidence) {
        String value = actorValue(spotRid, actor) + ";marker=" + request.marker();
        evidence.record("actor-fast-started", request.requestId(), value);
        evidence.record("actor-fast-completed", request.requestId(), value);
        return CompletableFuture.completedFuture(new Contracts.ActorFastRes(
            "ATD-B", request.requestId(), actor.actorId(), request.marker()));
    }

    private static String actorValue(String spotRid, AwaitActor actor) {
        return "actor=" + actor.actorId() + ";mailbox=actor:" + actor.actorId() + ";spot=" + spotRid;
    }


    private static void recordProbe(EvidenceStore evidence, AwaitProbeSpot spot, String requestId) {
        String value = spot.context().spotId();
        evidence.record("probe-started", requestId, value);
        evidence.record("probe-completed", requestId, value);
    }
}
