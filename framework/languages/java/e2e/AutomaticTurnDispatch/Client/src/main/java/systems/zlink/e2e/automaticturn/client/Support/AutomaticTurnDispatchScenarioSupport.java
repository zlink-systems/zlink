package systems.zlink.e2e.automaticturn.client.Support;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.client.ClientOptions;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class AutomaticTurnDispatchScenarioSupport {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final long ISOLATION_DELAY_MILLIS = 350;
    private static final long ACTOR_TIMER_ISOLATION_DELAY_MILLIS = 5000;
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ClientOptions options;

    public AutomaticTurnDispatchScenarioSupport(ClientOptions options) {
        this.options = options;
    }

    public void setDefaultPlacement() throws Exception {
        setPlacementWeights(100, 0);
    }

    public void runTerminatorSurface() {
        compileTimeTerminatorSurface();
    }

    private static void compileTimeTerminatorSurface() {
        // These method references make the exact Java interface contract a compile-time check.
        java.util.function.BiFunction<
            systems.zlink.framework.channels.ZLinkRequestCall,
            Class<Object>,
            CompletionStage<Object>> requestSubmit =
            systems.zlink.framework.channels.ZLinkRequestCall::submit;
        java.util.function.BiFunction<
            systems.zlink.framework.channels.ZLinkRequestCall,
            Class<Object>,
            CompletionStage<Object>> requestYield =
            systems.zlink.framework.channels.ZLinkRequestCall::yield;
        java.util.function.Function<
            systems.zlink.framework.actors.ZLinkActorJoinCall,
            systems.zlink.framework.actors.ZLinkActorJoinCall> joinTimeout =
            call -> call.timeout(Duration.ZERO);
        java.util.function.Consumer<systems.zlink.framework.actors.ZLinkActorJoinCall> joinDefer =
            systems.zlink.framework.actors.ZLinkActorJoinCall::defer;
        java.util.function.Function<
            systems.zlink.framework.spots.ZLinkWorkerCall<Object>,
            CompletionStage<Object>> workerSubmit =
            systems.zlink.framework.spots.ZLinkWorkerCall::submit;
        java.util.function.Function<
            systems.zlink.framework.spots.ZLinkWorkerCall<Object>,
            CompletionStage<Object>> workerYield =
            systems.zlink.framework.spots.ZLinkWorkerCall::yield;
        java.util.function.Function<
            systems.zlink.httpclient.ZLinkHttpServerRequestBuilder,
            CompletionStage<Void>> serverSubmit =
            systems.zlink.httpclient.ZLinkHttpServerRequestBuilder::submit;
        java.util.function.BiFunction<
            systems.zlink.httpclient.ZLinkHttpExecutionTurn,
            CompletionStage<Object>,
            CompletionStage<Object>> serverAsync =
            systems.zlink.httpclient.ZLinkHttpExecutionTurn::async;
        java.util.function.BiFunction<
            systems.zlink.httpclient.ZLinkHttpExecutionTurn,
            CompletionStage<Object>,
            CompletionStage<Object>> serverYield =
            systems.zlink.httpclient.ZLinkHttpExecutionTurn::yield;
        java.util.function.BiFunction<
            systems.zlink.httpclient.ZLinkHttpRequestBuilder,
            Class<Object>,
            CompletionStage<Object>> clientFetch =
            systems.zlink.httpclient.ZLinkHttpRequestBuilder::fetch;
        if (requestSubmit == null || requestYield == null || joinTimeout == null
            || joinDefer == null || workerSubmit == null || workerYield == null
            || serverSubmit == null || serverAsync == null || serverYield == null
            || clientFetch == null) {
            throw new IllegalStateException("TD-A1 terminator contract references were not created");
        }
    }

    public void runAsyncHoldsTurn(ZLinkStreamConnector connector) throws Exception {
        runTurnInterleave(connector, "TD-A2", "await-held", List.of(
            "await-held", "await-resumed", "completed", "probe-started", "probe-completed"));
    }

    public void runAsyncCompletion(ZLinkStreamConnector connector) throws Exception {
        String requestId = "tda4-" + System.nanoTime();
        sendTurnAwait(connector, "TD-A4", requestId);
        assertOrder(requestId, List.of("await-held", "await-resumed", "completed"));
    }

    public void runYieldReleasesTurn(ZLinkStreamConnector connector) throws Exception {
        runTurnInterleave(connector, "TD-B1", "yield-released", List.of(
            "yield-released", "probe-started", "probe-completed", "yield-resumed", "completed"));
    }

    public void runYieldQueuedOrder(ZLinkStreamConnector connector) throws Exception {
        String requestId = "tdb2-" + System.nanoTime();
        sendTurnAwait(connector, "TD-B2", requestId);
        assertOrder(requestId, List.of("yield-released"));
        for (int index = 1; index <= 3; index++) {
            connector.send(new Contracts.ProbeMsg(requestId, "probe-" + index))
                .metadata(Map.of(
                    Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
                    Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A))
                .submit();
        }
        assertOrder(requestId, List.of(
            "yield-released",
            "probe-started", "probe-completed",
            "probe-started", "probe-completed",
            "probe-started", "probe-completed",
            "yield-resumed", "completed"));
    }

    public void runCounterScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        String terminator) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector.send(new Contracts.CounterResetMsg(requestId, 0)).metadata(metadata).submit();
        waitMarkerCount(options.playHttpEndpoint() + "/evidence", requestId, "counter-reset", 1);
        for (int index = 0; index < 8; index++) {
            connector.send(new Contracts.CounterAwaitMsg(
                requestId, "op-" + index, 150, terminator)).metadata(metadata).submit();
        }
        waitMarkerCount(options.playHttpEndpoint() + "/evidence", requestId,
            "counter-operation-completed", 8);
        Contracts.CounterReadRes result = connector
            .request(new Contracts.CounterReadReq(requestId))
            .metadata(metadata)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.CounterReadRes.class).toCompletableFuture().join();
        ensure(result.value() == 8,
            scenarioId + " expected serialized counter value 8, actual " + result.value());
        ensure(observedMarkers(options.playHttpEndpoint() + "/evidence", requestId)
            .contains("counter-after-yield"), scenarioId + " did not observe continuation state");
    }

    public void runTimerTerminatorScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        String mode) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector.send(new Contracts.TimerStartMsg(
            requestId, requestId + "-timer", mode, 40, 350)).metadata(metadata).submit();
        if ("yield-then-next".equals(mode)) {
            assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
                List.of("yield-released", "yield-held", "yield-resumed", "yield-completed",
                    "timer-next-started", "timer-next-completed"));
        } else {
            assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
                List.of("timer-await-started", "timer-await-released",
                    "timer-await-resumed", "timer-await-completed"));
        }
        connector.send(new Contracts.TimerStopMsg(requestId)).metadata(metadata).submit();
    }

    public void runIoWorkerScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        String terminator) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector.send(new Contracts.IoWorkerMsg(requestId, 350, terminator))
            .metadata(metadata).submit();
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
            List.of("io-worker-started"));
        connector.send(new Contracts.ProbeMsg(requestId, scenarioId + "-probe"))
            .metadata(metadata).submit();
        if ("yield".equals(terminator)) {
            assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
                List.of("io-worker-started", "probe-started", "probe-completed",
                    "io-worker-resumed", "io-worker-completed"));
        } else {
            assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
                List.of("io-worker-started", "io-worker-resumed", "io-worker-completed",
                    "probe-started", "probe-completed"));
        }
    }

    public void runIoWorkerBatch(ZLinkStreamConnector connector, String scenarioId) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        Contracts.IoWorkerBatchRes result = connector
            .request(new Contracts.IoWorkerBatchReq(requestId, 16, 100))
            .metadata(metadata)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.IoWorkerBatchRes.class).toCompletableFuture().join();
        ensure(result.completed() == 16, scenarioId + " incomplete I/O worker batch");
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
            List.of("io-worker-batch-completed"));
    }

    public void runCpuWorkerScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        String terminator) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector.send(new Contracts.CpuWorkerMsg(requestId, 250, terminator))
            .metadata(metadata).submit();
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
            List.of("cpu-worker-started"));
        connector.send(new Contracts.ProbeMsg(requestId, scenarioId + "-probe"))
            .metadata(metadata).submit();
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId,
            "yield".equals(terminator)
                ? List.of("cpu-worker-started", "probe-started", "probe-completed",
                    "cpu-worker-resumed", "cpu-worker-completed")
                : List.of("cpu-worker-started", "cpu-worker-resumed", "cpu-worker-completed",
                    "probe-started", "probe-completed"));
    }

    private void waitMarkerCount(String evidenceUrl, String requestId, String marker, int count)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            int observed = 0;
            for (String value : observedMarkers(evidenceUrl, requestId)) {
                if (marker.equals(value)) observed++;
            }
            if (observed >= count) return;
            Thread.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + count + " " + marker);
    }

    private void runTurnInterleave(
        ZLinkStreamConnector connector,
        String scenarioId,
        String waitingMarker,
        List<String> expectedOrder) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        sendTurnAwait(connector, scenarioId, requestId);
        assertOrder(requestId, List.of(waitingMarker));
        connector.send(new Contracts.ProbeMsg(requestId, "turn-probe"))
            .metadata(Map.of(
                Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
                Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A))
            .submit();
        assertOrder(requestId, expectedOrder);
    }

    private void sendTurnAwait(
        ZLinkStreamConnector connector,
        String scenarioId,
        String requestId) {
        connector.send(new Contracts.AwaitMsg(requestId, 2_000, scenarioId))
            .metadata(Map.of(
                Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
                Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A))
            .submit();
    }

    public void runBasicTerminator(ZLinkStreamConnector connector) throws Exception {
        runScenario(connector, "ATD-A1", List.of(
            "hold-started",
            "hold-resumed",
            "hold-completed",
            "probe-started",
            "probe-completed"));
    }

    public void runAwaitTerminator(ZLinkStreamConnector connector) throws Exception {
        runScenario(connector, "ATD-A2", List.of(
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed",
            "probe-started",
            "probe-completed"));
    }

    public void runContinuationContext(ZLinkStreamConnector connector) throws Exception {
        runScenario(connector, "ATD-A3", List.of(
                "await-started",
                "await-released",
                "await-resumed",
                "await-completed"),
            Map.of(
                Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
                Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE),
            List.of(
                "spot=" + Contracts.TARGET_SPOT,
                "correlation=corr-a3"));
    }

    public void runActorOtherProgress(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdb1-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "ATD-B1 actor A bind mismatch");
        ensure(actorB.equals(bind.actorB()), "ATD-B1 actor B bind mismatch");

        joinActor(connector, requestId + "-join-a", actorA);
        joinActor(connector, requestId + "-join-b", actorB);

        CompletionStage<Contracts.ActorAwaitRes> await = connector
            .request(new Contracts.ActorAwaitReq(requestId, 800))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorAwaitRes.class);
        Thread.sleep(150);
        CompletionStage<Contracts.ActorFastRes> fast = connector
            .request(new Contracts.ActorFastReq(requestId, "b1-fast"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorB)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorFastRes.class);

        Contracts.ActorFastRes fastReply = fast.toCompletableFuture().join();
        Contracts.ActorAwaitRes awaitReply = await.toCompletableFuture().join();
        ensure(actorA.equals(awaitReply.actorId()), "ATD-B1 await actor mismatch");
        ensure(actorB.equals(fastReply.actorId()), "ATD-B1 fast actor mismatch");
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        assertOrder(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-fast-started",
            "actor-fast-completed",
            "actor-await-resumed",
            "actor-await-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed"), "actor=" + actorA);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-fast-started",
            "actor-fast-completed"), "actor=" + actorB);
    }

    public void runWorkerAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atda4-" + System.nanoTime();
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE);
        connector
            .send(new Contracts.WorkerAwaitMsg(requestId, 4000))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "worker-await-started",
            "worker-await-released"));
        connector
            .send(new Contracts.ProbeMsg(requestId, "worker-probe"))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "worker-await-started",
            "worker-await-released",
            "worker-await-resumed",
            "worker-await-completed",
            "probe-started",
            "probe-completed"));
    }

    public void runSameActorReentry(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdb2-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "ATD-B2 actor A bind mismatch");
        ensure(actorB.equals(bind.actorB()), "ATD-B2 actor B bind mismatch");

        joinActor(connector, requestId + "-join-a", actorA);

        CompletionStage<Contracts.ActorAwaitRes> await = connector
            .request(new Contracts.ActorAwaitReq(requestId, 350))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorAwaitRes.class);
        Thread.sleep(75);
        CompletionStage<Contracts.ActorFastRes> fast = connector
            .request(new Contracts.ActorFastReq(requestId, "b2-fast"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorFastRes.class);

        Contracts.ActorAwaitRes awaitReply = await.toCompletableFuture().join();
        Contracts.ActorFastRes fastReply = fast.toCompletableFuture().join();
        ensure(actorA.equals(awaitReply.actorId()), "ATD-B2 await actor mismatch");
        ensure(actorA.equals(fastReply.actorId()), "ATD-B2 fast actor mismatch");
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        assertOrder(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed",
            "actor-fast-started",
            "actor-fast-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed",
            "actor-fast-started",
            "actor-fast-completed"), "actor=" + actorA);
    }

    public void runActorJoinAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdb3-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "ATD-B3 actor A bind mismatch");
        ensure(actorB.equals(bind.actorB()), "ATD-B3 actor B bind mismatch");

        CompletionStage<Contracts.ActorJoinAwaitRes> join = connector
            .request(new Contracts.ActorJoinAwaitReq(requestId, Contracts.TARGET_SPOT))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorJoinAwaitRes.class);
        Thread.sleep(75);
        CompletionStage<Contracts.ActorFastRes> fast = connector
            .request(new Contracts.ActorFastReq(requestId, "b3-fast"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorB)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorFastRes.class);

        Contracts.ActorFastRes fastReply = fast.toCompletableFuture().join();
        Contracts.ActorJoinAwaitRes joinReply = join.toCompletableFuture().join();
        ensure(actorA.equals(joinReply.actorId()), "ATD-B3 join actor mismatch");
        ensure(actorB.equals(fastReply.actorId()), "ATD-B3 fast actor mismatch");
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        assertOrder(playEvidence, requestId, List.of(
            "actor-join-await-started",
            "actor-join-await-released",
            "actor-fast-started",
            "actor-fast-completed",
            "actor-join-await-resumed",
            "actor-join-await-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-join-await-started",
            "actor-join-await-released",
            "actor-join-await-resumed",
            "actor-join-await-completed"), "actor=" + actorA);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-fast-started",
            "actor-fast-completed"), "actor=" + actorB);
    }

    public void runUserSpotJoin(ZLinkStreamConnector connector) throws Exception {
        String requestId = "tde2-" + System.nanoTime();
        String spotA = requestId + "-spot-a";
        String spotB = requestId + "-spot-b";
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        ensureSpot(connector, spotA);
        ensureSpot(connector, spotB);
        bindActors(connector, spotA, actorA, actorB, "TD-E2");
        joinActor(connector, requestId + "-prepare-a", actorA, spotA);
        joinActor(connector, requestId + "-prepare-b", actorB, spotA);

        Contracts.ActorJoinRes joined = joinActor(
            connector, requestId + "-join", actorA, spotB);
        ensure(actorA.equals(joined.actorId()), "TD-E2 joined actor mismatch");
        ensure("joined".equals(joined.marker()), "TD-E2 join marker mismatch");
    }

    public void runOppositeUserSpotJoins(ZLinkStreamConnector connector) throws Exception {
        String requestId = "tde3-" + System.nanoTime();
        String spotA = requestId + "-spot-a";
        String spotB = requestId + "-spot-b";
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        ensureSpot(connector, spotA);
        ensureSpot(connector, spotB);
        bindActors(connector, spotA, actorA, actorB, "TD-E3");
        joinActor(connector, requestId + "-prepare-a", actorA, spotA);
        joinActor(connector, requestId + "-prepare-b-on-a", actorB, spotA);
        joinActor(connector, requestId + "-prepare-b", actorB, spotB);

        CompletionStage<Contracts.ActorJoinRes> aToB = requestActorJoin(
            connector, requestId + "-a-to-b", actorA, spotB);
        CompletionStage<Contracts.ActorJoinRes> bToA = requestActorJoin(
            connector, requestId + "-b-to-a", actorB, spotA);
        Contracts.ActorJoinRes joinedA = aToB.toCompletableFuture().join();
        Contracts.ActorJoinRes joinedB = bToA.toCompletableFuture().join();
        ensure(actorA.equals(joinedA.actorId()), "TD-E3 A to B actor mismatch");
        ensure(actorB.equals(joinedB.actorId()), "TD-E3 B to A actor mismatch");
        ensure("joined".equals(joinedA.marker()), "TD-E3 A to B marker mismatch");
        ensure("joined".equals(joinedB.marker()), "TD-E3 B to A marker mismatch");
    }

    public void runTimerIsolation(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdc1-" + System.nanoTime();
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE);
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                requestId + "-await",
                "await-on-first",
                50,
                ISOLATION_DELAY_MILLIS))
            .metadata(metadata)
            .submit()
            .toCompletableFuture()
            .join();
        awaitEvidenceMarker(
            playEvidence + "/wait?marker=timer-await-released&subject="
                + requestId + "&timeoutMs=30000",
            requestId,
            "timer-await-released");
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                requestId + "-fast",
                "fast",
                100,
                0))
            .metadata(metadata)
            .submit()
            .toCompletableFuture()
            .join();
        assertOrder(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed",
            "timer-fast-started",
            "timer-fast-completed"));
        connector
            .send(new Contracts.TimerStopMsg(requestId))
            .metadata(metadata)
            .submit();
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed"), "timer=" + requestId + "-await");
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timer-fast-started",
            "timer-fast-completed"), "timer=" + requestId + "-fast");
    }

    public void runSameTimerReentry(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdc2-" + System.nanoTime();
        String timerName = requestId + "-same";
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE);
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                timerName,
                "await-then-next",
                50,
                1000))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed",
            "timer-next-started",
            "timer-next-completed"));
        connector
            .send(new Contracts.TimerStopMsg(requestId))
            .metadata(metadata)
            .submit();
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed",
            "timer-next-started",
            "timer-next-completed"), "timer=" + timerName);
    }

    public void runActorTimerIsolation(ZLinkStreamConnector connector) throws Exception {
        String scenarioId = "atdc3-" + System.nanoTime();
        String actorA = scenarioId + "-actor-a";
        String actorB = scenarioId + "-actor-b";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "ATD-C3 actor A bind mismatch");
        ensure(actorB.equals(bind.actorB()), "ATD-C3 actor B bind mismatch");

        joinActor(connector, scenarioId + "-join-a", actorA);
        joinActor(connector, scenarioId + "-join-b", actorB);

        runActorThenTimer(connector, scenarioId + "-actor-then-timer", actorA);
        runTimerThenActor(connector, scenarioId + "-timer-then-actor", actorB);
    }

    private void runActorThenTimer(
        ZLinkStreamConnector connector,
        String requestId,
        String actorId) throws Exception {
        Map<String, String> timerMetadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE);
        String timerName = requestId + "-fast";
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        CompletionStage<Contracts.ActorAwaitRes> actorAwait = connector
            .request(new Contracts.ActorAwaitReq(requestId, ACTOR_TIMER_ISOLATION_DELAY_MILLIS))
            .metadata(Contracts.ACTOR_ID_METADATA, actorId)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorAwaitRes.class);
        assertOrder(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released"));
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                timerName,
                "fast",
                50,
                0))
            .metadata(timerMetadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "timer-fast-started",
            "timer-fast-completed"));
        connector
            .send(new Contracts.TimerStopMsg(requestId))
            .metadata(timerMetadata)
            .submit();
        Contracts.ActorAwaitRes reply = actorAwait.toCompletableFuture().join();
        ensure(actorId.equals(reply.actorId()), "ATD-C3 actor await reply mismatch");
        assertOrder(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed",
            "timer-fast-started",
            "timer-fast-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed"), "actor=" + actorId);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timer-fast-started",
            "timer-fast-completed"), "timer=" + timerName);
    }

    private void runTimerThenActor(
        ZLinkStreamConnector connector,
        String requestId,
        String actorId) throws Exception {
        Map<String, String> timerMetadata = Map.of(
            Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE);
        String timerName = requestId + "-await";
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                timerName,
                "await-on-first",
                50,
                ACTOR_TIMER_ISOLATION_DELAY_MILLIS))
            .metadata(timerMetadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released"));
        Contracts.ActorFastRes reply = connector
            .request(new Contracts.ActorFastReq(requestId, "c3-actor-fast"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorId)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorFastRes.class).toCompletableFuture().join();
        ensure(actorId.equals(reply.actorId()), "ATD-C3 actor fast reply mismatch");
        assertOrder(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed",
            "actor-fast-started",
            "actor-fast-completed"));
        connector
            .send(new Contracts.TimerStopMsg(requestId))
            .metadata(timerMetadata)
            .submit();
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed"), "timer=" + timerName);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-fast-started",
            "actor-fast-completed"), "actor=" + actorId);
    }

    public void runRemoteSpotAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdd2-" + System.nanoTime();
        String ownerSpot = requestId + "-owner";
        String targetSpot = requestId + "-target";
        String playAEvidence = options.playHttpEndpoint() + "/evidence";
        String playBEvidence = options.playBHttpEndpoint() + "/evidence";
        try {
            setPlacementWeights(100, 0);
            Contracts.EnsureSpotRes owner = connector
                .request(new Contracts.EnsureSpotReq(ownerSpot))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
            ensure(ownerSpot.equals(owner.spotRid()), "ATD-D2 owner spot mismatch");
            ensure(Contracts.PLAY_NODE_A.equals(owner.nodeRid()), "ATD-D2 owner node mismatch");

            setPlacementWeights(0, 100);
            Contracts.EnsureSpotRes target = connector
                .request(new Contracts.EnsureSpotReq(targetSpot))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
            ensure(targetSpot.equals(target.spotRid()), "ATD-D2 target spot mismatch");
            ensure(Contracts.PLAY_NODE_B.equals(target.nodeRid()), "ATD-D2 target node mismatch");

            setPlacementWeights(100, 0);
            Contracts.ScenarioRes reply = connector
                .request(new Contracts.RemoteSpotAwaitReq(requestId, targetSpot, 350))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
                .metadata(Contracts.SPOT_RID_METADATA, ownerSpot)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.ScenarioRes.class).toCompletableFuture().join();
            ensure("ATD-D2".equals(reply.scenarioId()), "ATD-D2 reply scenario mismatch");
            ensure(requestId.equals(reply.requestId()), "ATD-D2 reply request mismatch");
            ensure(Contracts.PLAY_NODE_A.equals(reply.result()), "ATD-D2 owner continuation node mismatch");

            assertOrder(playAEvidence, requestId, List.of(
                "remote-await-started",
                "remote-await-released",
                "remote-await-resumed",
                "remote-await-completed"));
            assertAllValuesContain(playAEvidence, requestId, List.of(
                "remote-await-started",
                "remote-await-released",
                "remote-await-resumed",
                "remote-await-completed"), "spot=" + ownerSpot);
            assertAllValuesContain(playAEvidence, requestId, List.of(
                "remote-await-resumed",
                "remote-await-completed"), "targetNode=" + Contracts.PLAY_NODE_B);
            assertOrder(playBEvidence, requestId, List.of(
                "await-started",
                "await-released",
                "await-resumed",
                "await-completed"));
            assertAllValuesContain(playBEvidence, requestId, List.of(
                "await-started",
                "await-released",
                "await-resumed",
                "await-completed"), "spot=" + targetSpot);
            assertNoMarker(playBEvidence, requestId, "remote-await-resumed");
        } finally {
            setPlacementWeights(100, 0);
        }
    }

    public void runRouteBridgeAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdd3-" + System.nanoTime();
        String spotRid = requestId + "-spot";
        String playBEvidence = options.playBHttpEndpoint() + "/evidence";
        setPlacementWeights(0, 100);
        Contracts.EnsureSpotRes target;
        try {
            target = connector
                .request(new Contracts.EnsureSpotReq(spotRid))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        } finally {
            setPlacementWeights(100, 0);
        }
        ensure(spotRid.equals(target.spotRid()), "ATD-D3 target spot mismatch");
        ensure(Contracts.PLAY_NODE_B.equals(target.nodeRid()), "ATD-D3 target node mismatch");

        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, spotRid,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B);
        waitRouteBridgeSendReady(connector, playBEvidence, requestId + "-ready", metadata);
        connector
            .send(new Contracts.AwaitMsg(requestId, ISOLATION_DELAY_MILLIS, "route-bridge"))
            .metadata(metadata)
            .submit();
        assertOrder(playBEvidence, requestId, List.of(
            "await-started",
            "await-released"));
        connector
            .send(new Contracts.ProbeMsg(requestId, "route-bridge-probe"))
            .metadata(metadata)
            .submit();
        assertOrder(playBEvidence, requestId, List.of(
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed",
            "probe-started",
            "probe-completed"));
        assertAllValuesContain(playBEvidence, requestId, List.of(
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed"), "spot=" + spotRid);
        assertAllValuesContain(playBEvidence, requestId, List.of(
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed"), "handler=spot");
        assertAllValuesContain(playBEvidence, requestId, List.of(
            "probe-started",
            "probe-completed"), spotRid);
    }

    private void waitRouteBridgeSendReady(
        ZLinkStreamConnector connector,
        String evidenceUrl,
        String requestId,
        Map<String, String> metadata) throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        int attempt = 0;
        while (System.nanoTime() < deadline) {
            String probeId = requestId + "-" + attempt++;
            connector
                .send(new Contracts.ProbeMsg(probeId, "route-bridge-ready"))
                .metadata(metadata)
                .submit();
            long probeDeadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (System.nanoTime() < probeDeadline) {
                if (containsInOrder(
                    observedMarkers(evidenceUrl, probeId),
                    List.of("probe-started", "probe-completed"))) {
                    return;
                }
                Thread.sleep(100);
            }
        }
        throw new IllegalStateException("route bridge send path was not ready for " + requestId);
    }

    public void runSessionRelayActorAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atdd4-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "ATD-D4 actor A bind mismatch");

        ZLinkStreamConnector unbound = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(options.streamEndpoint())));
        try {
            unbound.connect().submit().toCompletableFuture().join();
            CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
                .waitFor(Contracts.ActorPushNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorPushAwaitRes reply = connector
                .request(new Contracts.ActorPushAwaitReq(requestId, 350, "bound-session-push"))
                .metadata(Contracts.ACTOR_ID_METADATA, actorA)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.ActorPushAwaitRes.class).toCompletableFuture().join();
            connector.dispatch().submit().toCompletableFuture().join();
            Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
            ensure("ATD-D4".equals(reply.scenarioId()), "ATD-D4 reply scenario mismatch");
            ensure(actorA.equals(reply.actorId()), "ATD-D4 reply actor mismatch");
            ensure("actor-push-await-completed".equals(reply.marker()), "ATD-D4 reply marker mismatch");
            ensure(actorA.equals(notify.actorId()), "ATD-D4 push actor mismatch");
            ensure(requestId.equals(notify.requestId()), "ATD-D4 push request mismatch");
            ensure("bound-session-push".equals(notify.value()), "ATD-D4 push value mismatch");
            ensure(Contracts.PLAY_NODE_A.equals(notify.nodeRid()), "ATD-D4 push node mismatch");
            CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> unboundPush = unbound
                .waitFor(Contracts.ActorPushNotify.class)
                .timeout(Duration.ofMillis(400))
                .submit(Contracts.ActorPushNotify.class);
            unbound.dispatch().submit().toCompletableFuture().join();
            expectFailure(() -> unboundPush.toCompletableFuture().join(), "ATD-D4 unbound session received actor push");
        } finally {
            unbound.close().submit().toCompletableFuture().join();
        }

        assertOrder(playEvidence, requestId, List.of(
            "actor-push-await-started",
            "actor-push-await-released",
            "actor-push-await-resumed",
            "actor-push-await-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "actor-push-await-started",
            "actor-push-await-released",
            "actor-push-await-resumed",
            "actor-push-await-completed"), "actor=" + actorA);
    }

    public void runTimeoutCleanup(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atde1-" + System.nanoTime();
        String spotRid = requestId + "-spot";
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        Contracts.EnsureSpotRes target = connector
            .request(new Contracts.EnsureSpotReq(spotRid))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        ensure(spotRid.equals(target.spotRid()), "ATD-E1 spot mismatch");
        ensure(Contracts.PLAY_NODE_A.equals(target.nodeRid()), "ATD-E1 node mismatch");

        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, spotRid,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector
            .send(new Contracts.AwaitTimeoutMsg(requestId, 700, 100))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "timeout-await-started",
            "timeout-await-released",
            "timeout-await-completed"));
        assertNoMarker(playEvidence, requestId, "timeout-await-unexpected-resumed");
        Contracts.ProbeRes timeoutProbe = connector
            .request(new Contracts.ProbeReq(requestId))
            .metadata(metadata)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ProbeRes.class).toCompletableFuture().join();
        ensure(requestId.equals(timeoutProbe.requestId()), "ATD-E1 probe reply mismatch");
        assertOrder(playEvidence, requestId, List.of(
            "timeout-await-started",
            "timeout-await-released",
            "timeout-await-completed",
            "probe-started",
            "probe-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "timeout-await-started",
            "timeout-await-released",
            "timeout-await-completed"), "spot=" + spotRid);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "probe-started",
            "probe-completed"), spotRid);
    }

    public void runCancellationCleanup(ZLinkStreamConnector connector) throws Exception {
        String requestId = "atde2-" + System.nanoTime();
        String spotRid = requestId + "-spot";
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        Contracts.EnsureSpotRes target = connector
            .request(new Contracts.EnsureSpotReq(spotRid))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        ensure(spotRid.equals(target.spotRid()), "ATD-E2 spot mismatch");
        ensure(Contracts.PLAY_NODE_A.equals(target.nodeRid()), "ATD-E2 node mismatch");

        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, spotRid,
            Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A);
        connector
            .send(new Contracts.AwaitCancelMsg(requestId, 700, 100))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId, List.of(
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed"));
        assertNoMarker(playEvidence, requestId, "cancel-await-unexpected-resumed");
        Contracts.ProbeRes cancellationProbe = connector
            .request(new Contracts.ProbeReq(requestId))
            .metadata(metadata)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ProbeRes.class).toCompletableFuture().join();
        ensure(requestId.equals(cancellationProbe.requestId()), "ATD-E2 probe reply mismatch");
        assertOrder(playEvidence, requestId, List.of(
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed",
            "probe-started",
            "probe-completed"));
        assertAllValuesContain(playEvidence, requestId, List.of(
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed"), "spot=" + spotRid);
        assertAllValuesContain(playEvidence, requestId, List.of(
            "probe-started",
            "probe-completed"), spotRid);
    }

    public void runShutdownWait(ZLinkStreamConnector connector) throws Exception {
        String requestId = options.requiredShutdownRequestId();
        String spotRid = options.requiredShutdownSpotRid();
        try {
            Contracts.AwaitShutdownRes result = connector
                .request(new Contracts.AwaitShutdownScenarioReq(requestId, spotRid, 30_000))
                .timeout(Duration.ofSeconds(90))
                .submit(Contracts.AwaitShutdownRes.class).toCompletableFuture().join();
            throw new IllegalStateException(
                "ATD-E3 expected play-a shutdown while await was pending, but request completed as "
                    + result.operation());
        } catch (RuntimeException error) {
            ensure(!String.valueOf(error.getMessage()).contains("unexpected-completion"),
                String.valueOf(error.getMessage()));
            System.out.println("automatic-turn-dispatch shutdown wait result=passed");
        }
    }

    public void runShutdownWaitAndRecovery(
        ZLinkStreamConnector connector,
        ZLinkStreamConnector recovery) throws Exception {
        String requestId = options.requiredShutdownRequestId();
        String spotRid = options.requiredShutdownSpotRid();
        CompletionStage<Contracts.AwaitShutdownRes> pending = connector
            .request(new Contracts.AwaitShutdownScenarioReq(requestId, spotRid, 30_000))
            .timeout(Duration.ofSeconds(90))
            .submit(Contracts.AwaitShutdownRes.class);
        awaitEvidenceMarker(
            options.playHttpEndpoint() + "/evidence/wait?marker=await-released&subject="
                + requestId + "&timeoutMs=30000",
            requestId,
            "await-released");
        touchControlFile("atd-e3-ready-to-stop-play");
        expectFailure(pending, "ATD-E3 pending await unexpectedly completed");
        waitForControlFile("atd-e3-play-restarted");
        runShutdownRecovery(recovery, requestId, spotRid);
    }

    public void runShutdownRecovery(ZLinkStreamConnector connector) throws Exception {
        String requestId = options.requiredShutdownRequestId();
        String spotRid = options.requiredShutdownSpotRid();
        runShutdownRecovery(connector, requestId, spotRid);
    }

    private void runShutdownRecovery(
        ZLinkStreamConnector connector,
        String requestId,
        String spotRid) throws Exception {
        Contracts.AwaitShutdownRes result = connector
            .request(new Contracts.AwaitShutdownRecoveryReq(requestId, spotRid))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.AwaitShutdownRes.class).toCompletableFuture().join();
        ensure("atd.e3-shutdown-recovery".equals(result.operation()), "ATD-E3 recovery operation mismatch");
        ensure(requestId.equals(result.requestId()), "ATD-E3 recovery request mismatch");
        ensure(spotRid.equals(result.spotRid()), "ATD-E3 recovery spot mismatch");
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId, List.of(
            "probe-started",
            "probe-completed"));
        System.out.println("automatic-turn-dispatch shutdown recovery result=passed");
    }

    private void touchControlFile(String fileName) throws Exception {
        Path directory = Path.of(options.requiredControlDirectory());
        Files.createDirectories(directory);
        Files.writeString(directory.resolve(fileName), "ready\n");
    }

    private void waitForControlFile(String fileName) throws Exception {
        Path file = Path.of(options.requiredControlDirectory()).resolve(fileName);
        long deadline = System.nanoTime() + Duration.ofSeconds(70).toNanos();
        while (System.nanoTime() < deadline) {
            if (Files.exists(file)) {
                return;
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for ATD-E3 control file " + file);
    }

    private static void expectFailure(
        CompletionStage<?> stage,
        String unexpectedCompletionMessage) {
        try {
            stage.toCompletableFuture().get(20, TimeUnit.SECONDS);
        } catch (Exception expected) {
            return;
        }
        throw new IllegalStateException(unexpectedCompletionMessage);
    }

    public void runReadinessProbe(ZLinkStreamConnector connector) throws Exception {
        try {
            setPlacementWeights(100, 0);
            Contracts.EnsureSpotRes playA = connector
                .request(new Contracts.EnsureSpotReq(Contracts.TARGET_SPOT))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
            ensure(Contracts.TARGET_SPOT.equals(playA.spotRid()), "readiness play-a spot mismatch");
            ensure(Contracts.PLAY_NODE_A.equals(playA.nodeRid()), "readiness play-a node mismatch");

            setPlacementWeights(0, 100);
            String playBSpot = Contracts.TARGET_SPOT + "-readiness-b";
            Contracts.EnsureSpotRes playB = connector
                .request(new Contracts.EnsureSpotReq(playBSpot))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
            ensure(playBSpot.equals(playB.spotRid()), "readiness play-b spot mismatch");
            ensure(Contracts.PLAY_NODE_B.equals(playB.nodeRid()), "readiness play-b node mismatch");
        } finally {
            setPlacementWeights(100, 0);
        }
    }

    private void joinActor(
        ZLinkStreamConnector connector,
        String requestId,
        String actorId) throws Exception {
        Contracts.ActorJoinRes reply = joinActor(
            connector, requestId, actorId, Contracts.TARGET_SPOT);
        ensure(actorId.equals(reply.actorId()), "ATD-B1 join actor mismatch");
        ensure("joined".equals(reply.marker()), "ATD-B1 join marker mismatch");
    }

    private Contracts.ActorJoinRes joinActor(
        ZLinkStreamConnector connector,
        String requestId,
        String actorId,
        String targetSpotRid) throws Exception {
        Contracts.ActorJoinRes reply = requestActorJoin(connector, requestId, actorId, targetSpotRid)
            .toCompletableFuture().join();
        awaitEvidenceMarker(
            options.playHttpEndpoint() + "/evidence/wait?marker=actor-target-joined&subject="
                + actorId + "&timeoutMs=30000",
            actorId,
            "actor-target-joined");
        return reply;
    }

    private CompletionStage<Contracts.ActorJoinRes> requestActorJoin(
        ZLinkStreamConnector connector,
        String requestId,
        String actorId,
        String targetSpotRid) {
        return connector
            .request(new Contracts.ActorJoinReq(requestId, targetSpotRid))
            .metadata(Contracts.ACTOR_ID_METADATA, actorId)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorJoinRes.class);
    }

    private void ensureSpot(ZLinkStreamConnector connector, String spotRid) {
        Contracts.EnsureSpotRes ensured = connector
            .request(new Contracts.EnsureSpotReq(spotRid))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        ensure(spotRid.equals(ensured.spotRid()), "ensured spot mismatch: " + spotRid);
    }

    private void bindActors(
        ZLinkStreamConnector connector,
        String spotRid,
        String actorA,
        String actorB,
        String scenarioId) {
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(spotRid, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), scenarioId + " actor A bind mismatch");
        ensure(actorB.equals(bind.actorB()), scenarioId + " actor B bind mismatch");
    }

    private void runScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        List<String> expectedOrder) throws Exception {
        runScenario(connector, scenarioId, expectedOrder, Map.of(), List.of());
    }

    private void runScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        List<String> expectedOrder,
        Map<String, String> metadata,
        List<String> expectedValueFragments) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Contracts.ScenarioRes reply = connector
            .request(new Contracts.ScenarioReq(scenarioId, requestId))
            .metadata(metadata)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ScenarioRes.class).toCompletableFuture().join();
        ensure(scenarioId.equals(reply.scenarioId()), scenarioId + " reply scenario mismatch");
        ensure(requestId.equals(reply.requestId()), scenarioId + " reply request id mismatch");
        assertOrder(requestId, expectedOrder);
        for (String valueFragment : expectedValueFragments) {
            assertAllValuesContain(requestId, expectedOrder, valueFragment);
        }
    }

    private void assertOrder(String requestId, List<String> expectedOrder) throws Exception {
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId, expectedOrder);
    }

    private void assertOrder(String evidenceUrl, String requestId, List<String> expectedOrder)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            List<String> observed = observedMarkers(evidenceUrl, requestId);
            if (containsInOrder(observed, expectedOrder)) {
                return;
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException(
            "expected marker order " + expectedOrder + " for " + requestId
                + ", observed=" + observedMarkers(evidenceUrl, requestId));
    }

    private List<String> observedMarkers(String requestId) throws Exception {
        return observedMarkers(options.playHttpEndpoint() + "/evidence", requestId);
    }

    private List<String> observedMarkers(String evidenceUrl, String requestId) throws Exception {
        JsonNode root = JSON.readTree(get(evidenceUrl));
        List<String> markers = new ArrayList<>();
        for (JsonNode entry : root.path("entries")) {
            if (requestId.equals(entry.path("subject").asText())) {
                markers.add(entry.path("marker").asText());
            }
        }
        return markers;
    }

    private void assertAllValuesContain(
        String requestId,
        List<String> expectedMarkers,
        String valueFragment) throws Exception {
        assertAllValuesContain(
            options.playHttpEndpoint() + "/evidence",
            requestId,
            expectedMarkers,
            valueFragment);
    }

    private void assertAllValuesContain(
        String evidenceUrl,
        String requestId,
        List<String> expectedMarkers,
        String valueFragment) throws Exception {
        JsonNode root = JSON.readTree(get(evidenceUrl));
        for (JsonNode entry : root.path("entries")) {
            String marker = entry.path("marker").asText();
            if (requestId.equals(entry.path("subject").asText()) && expectedMarkers.contains(marker)) {
                String value = entry.path("value").asText();
                ensure(value.contains(valueFragment),
                    "expected " + marker + " value to contain " + valueFragment + ", value=" + value);
            }
        }
    }

    private void assertNoMarker(String evidenceUrl, String requestId, String marker) throws Exception {
        List<String> observed = observedMarkers(evidenceUrl, requestId);
        ensure(!observed.contains(marker), "unexpected marker " + marker + " for " + requestId);
    }

    private void waitForMetricValue(String metricsUrl, String metricName, double minimum)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            JsonNode rows = JSON.readTree(get(metricsUrl));
            for (JsonNode row : rows) {
                if (metricName.equals(row.path("name").asText())
                    && row.path("value").asDouble() >= minimum) {
                    return;
                }
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException(
            "timed out waiting for " + metricName + " >= " + minimum);
    }

    private interface ThrowingRunnable {
        void run() throws Exception;
    }

    private void expectFailure(ThrowingRunnable action, String message) throws Exception {
        try {
            action.run();
        } catch (Exception error) {
            if (error instanceof ExecutionException && error.getCause() != null) {
                return;
            }
            return;
        }
        throw new IllegalStateException(message);
    }

    private static boolean containsInOrder(List<String> observed, List<String> expected) {
        int index = 0;
        for (String marker : observed) {
            if (index < expected.size() && expected.get(index).equals(marker)) {
                index++;
            }
        }
        return index == expected.size();
    }

    private String get(String url) throws Exception {
        return get(url, Duration.ofSeconds(3));
    }

    private String get(String url, Duration timeout) throws Exception {
        URI target = URI.create(url);
        String baseUrl = target.getScheme() + "://" + target.getRawAuthority();
        String path = target.getRawPath();
        if (target.getRawQuery() != null) {
            path += "?" + target.getRawQuery();
        }
        RawHttpResponse response = ZLinkHttpClient.create(baseUrl)
            .timeout(timeout)
            .get(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "GET " + url + " returned " + response.status());
        return response.body();
    }

    private String post(String url, Duration timeout) throws Exception {
        URI target = URI.create(url);
        String baseUrl = target.getScheme() + "://" + target.getRawAuthority();
        String path = target.getRawPath();
        if (target.getRawQuery() != null) {
            path += "?" + target.getRawQuery();
        }
        RawHttpResponse response = ZLinkHttpClient.create(baseUrl)
            .timeout(timeout)
            .post(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "HTTP write " + url + " returned " + response.status());
        return response.body();
    }

    private void setPlacementWeights(int playAWeight, int playBWeight) throws Exception {
        post(options.playHttpEndpoint() + "/placement-weight?weight=" + playAWeight,
            REQUEST_TIMEOUT);
        post(options.playBHttpEndpoint() + "/placement-weight?weight=" + playBWeight,
            REQUEST_TIMEOUT);
    }

    private void awaitEvidenceMarker(String url, String subject, String marker) throws Exception {
        JsonNode result = JSON.readTree(get(url, REQUEST_TIMEOUT));
        ensure(result.path("matched").asBoolean(false),
            "timed out waiting for " + marker + " for " + subject);
    }

    private void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
