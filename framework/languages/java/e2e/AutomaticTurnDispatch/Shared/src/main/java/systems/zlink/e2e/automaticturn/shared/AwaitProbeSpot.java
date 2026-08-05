package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;

public final class AwaitProbeSpot implements ZLinkSpot<AwaitActor> {
    private final ZLinkSpotContext context;
    private final EvidenceStore evidence;
    private final PersistentRoomEvents persistentEvents;
    private String persistentValue = "";
    private int persistentEventCount;
    private boolean replayed;
    private final Map<String, TimerScenario> timerScenarios = new HashMap<>();
    private final Map<String, ZLinkTimer> timers = new HashMap<>();

    public AwaitProbeSpot(
        ZLinkSpotContext context,
        EvidenceStore evidence,
        PersistentRoomEvents persistentEvents) {
        this.context = context;
        this.evidence = evidence;
        this.persistentEvents = persistentEvents;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(AwaitProbeHandlers.HoldHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.AwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.WorkerAwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ProbeHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.WorkerAwaitMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ProbeMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.AwaitMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.AwaitTimeoutMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.AwaitCancelMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.ObservabilityQueueHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.RemoteSpotAwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.TimerStartMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.TimerStopMsgHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.SpotActorAwaitHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.SpotActorFastHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.SpotActorJoinHandler.class);
        context.handlers().addHandler(AwaitProbeHandlers.PersistentRoomStateHandler.class);
    }

    public synchronized CompletionStage<Void> startTimer(Contracts.TimerStartMsg command) {
        ZLinkTimer previous = timers.remove(command.timerName());
        if (previous != null) {
            previous.close();
        }
        timerScenarios.put(command.timerName(), new TimerScenario(
            command.requestId(),
            command.mode(),
            command.delayMillis()));
        ZLinkTimerOptions options = new ZLinkTimerOptions(
            ZLinkTimerOverrunPolicy.DELAY_NEXT_TICK,
            1,
            true);
        return context.addTimer(
                command.timerName(),
                Duration.ofMillis(command.periodMillis()),
                AwaitProbeHandlers.TimerTickHandler.class,
                options)
            .thenAccept(timer -> {
                synchronized (this) {
                    timers.put(command.timerName(), timer);
                }
            });
    }

    public synchronized void stopTimers(String requestId) {
        timerScenarios.entrySet().removeIf(entry -> {
            if (!entry.getValue().requestId().equals(requestId)) {
                return false;
            }
            ZLinkTimer timer = timers.remove(entry.getKey());
            if (timer != null) {
                timer.close();
            }
            return true;
        });
    }

    public synchronized TimerScenario timerScenario(String timerName) {
        return timerScenarios.get(timerName);
    }

    public synchronized void closeTimer(String timerName) {
        timerScenarios.remove(timerName);
        ZLinkTimer timer = timers.remove(timerName);
        if (timer != null) {
            timer.close();
        }
    }

    public record TimerScenario(String requestId, String mode, long delayMillis) {
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        java.util.List<String> events = persistentEvents.replay(context.spotId());
        persistentEventCount = events.size();
        persistentValue = events.isEmpty() ? "" : events.get(events.size() - 1);
        replayed = !events.isEmpty();
        evidence.record("persistent-room-replayed", context.spotId(),
            "events=" + persistentEventCount + ";value=" + persistentValue);
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    public synchronized Contracts.PersistentRoomStateRes persistentState(
        Contracts.PersistentRoomStateReq request) {
        java.util.List<String> events = request.append()
            ? persistentEvents.appendAndReplay(context.spotId(), request.value())
            : persistentEvents.replay(context.spotId());
        persistentEventCount = events.size();
        persistentValue = events.isEmpty() ? "" : events.get(events.size() - 1);
        return new Contracts.PersistentRoomStateRes(
            context.spotId(), persistentValue, persistentEventCount,
            replayed, evidence.nodeRid());
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        if (actorId.startsWith("atdb3-")) {
            Contracts.DelayReq delay = request.decode(Contracts.DelayReq.class);
            try {
                Thread.sleep(delay.delayMillis());
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("actor join interrupted", error);
            }
        }
        evidence.record("actor-target-join-requested", actorId, context.spotId());
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(AwaitActor actor) {
        evidence.record("actor-target-joined", actor.actorId(), context.spotId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(AwaitActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
